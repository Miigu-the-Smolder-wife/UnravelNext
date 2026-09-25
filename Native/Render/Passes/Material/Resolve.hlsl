// unx-kernel: cs_6_6 main
// unx-variants: DEBUG=0,1 PLANAR_MASK=0,1
// Material resolve (ARCHITECTURE 2.2), one 8 x 8 tile per group:
//   vis id -> surface (MaterialSurface.hlsli: exact pixel-centre barycentrics and their screen derivatives)
//   -> footprint-filtered textures (SampleGrad, anisotropic 16x) -> shading normal and band-limited roughness
//   -> G-buffer (GBuffer.hlsli), material word (MaterialInternal.hlsli), emissive (textured materials only),
//      tile class lists for the shading kernels, reflection lobe tiles for R (INTERFACES 5.1, v1.3).
// Specular band limit: the lobe is widened by the slope variance of the pixel footprint (Beckmann-equivalent
// alpha'^2 = alpha^2 + trace of the slope covariance):
//   - normal map: LEAN slope moments filtered by the hardware over the footprint (mNormalMoments);
//   - geometry: the interpolated normal varies linearly over the pixel box, trace = (|dn/dx|^2 + |dn/dy|^2) / 12.
// P[0] = { visId SRV, visibleClusters SRV, gbuffer UAV, material word UAV }
// P[1] = { emissive UAV or UNX_NONE, lobe tiles UAV, tile lists UAV (raw), tile args UAV (raw) }
// P[2] = { texture table SRV, tilesX, tilesY, tileCount }
// P[4] = { screen bands, view height, 0, 0 }: the class tile lists are split by band (MaterialSystem.h ResolveOutputs):
//        class c, band b at entry c * tileCount + tilesX * (row(b) / 8), arguments at 12 (c * bands + b), where row(b) is
//        RenderGraph::addBandedGroup's split min(H, floor(H b / bands) & ~7).
// P[3].y experiment mask (material.experiment_disable: cost attribution only, 0 otherwise)
// PLANAR_MASK=1 (planar reflection views with R's mask; views without one compile none of it):
// P[3].z R's planar tile mask (R8_UINT per 8 x 8 tile, nonzero = mirror pixels; UNX_NONE = absent), P[3].w R's planar
//        pixel mask (R8_UINT, read when there is no tile mask; UNX_NONE = absent). A planar reflection view's tile without
//        mirror pixels (V left them VIS_NONE) goes to no class list, so no shading kernel runs there (v1.22); its pixels
//        still get the sky word the neighbours' edge detection reads.
// P[3].x (DEBUG=1) RWStructuredBuffer<float4>, 3 per pixel: (uv, duv/dx), (duv/dy, variance, roughness'),
//        (camera-relative hit, front)
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

#define M_PI 3.14159265358979

groupshared uint gs_classMask;
groupshared uint gs_minLobe;
#if PLANAR_MASK
groupshared uint gs_mirror;
#endif

[numthreads(8, 8, 1)]
void main(uint2 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        gs_classMask = 0;
        gs_minLobe = asuint(1.0);
#if PLANAR_MASK
        gs_mirror = 1;
        if (P[3].z != UNX_NONE)
        {
            Texture2D<uint> tileMask = ResourceDescriptorHeap[P[3].z];
            gs_mirror = tileMask[gid] != 0 ? 1 : 0;
        }
        else if (P[3].w != UNX_NONE) gs_mirror = 0;  // any mirror pixel of the tile sets it below
#endif
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = gid * M_TILE + tid;
    uint classBit = 0;
    float lobe = 1.0;  // sky and outside the view: never the tile minimum below a surface pixel
    if (all(pixel < uint2(g_viewWidth, g_viewHeight)))
    {
        Texture2D<uint> visIds = ResourceDescriptorHeap[P[0].x];
        RWTexture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
        RWTexture2D<uint> words = ResourceDescriptorHeap[P[0].w];
        const uint visId = visIds[pixel];
        if (visId == VIS_NONE)
        {
            gbuffer[pixel] = uint2(0, 0);
            words[pixel] = M_MATERIAL_SKY;
            classBit = 1u << M_CLASS_SKY;
        }
        else if ((P[3].y & 4) != 0)
        {
            gbuffer[pixel] = uint2(0x7FFF0000u, 0x80808080u);
            words[pixel] = 0;
            classBit = 1u << M_CLASS_OPAQUE;
            lobe = 0.5;
        }
        else
        {
            const MSurface s = mSurfaceFromVis(visId, P[0].y, float2(pixel) + 0.5);
            const GpuMaterial m = loadMaterial(s.material);
            const MTextureSet ts = mLoadTextureSet(P[2].x, s.material);

            float3 baseColor = m.baseColor;
            if (ts.baseColor != UNX_NONE && (P[3].y & 1) == 0)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
                baseColor *= mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, s.uv, s.duvdx, s.duvdy).rgb;
            }
            float roughness = m.roughness, metallic = m.metallic;
            if (ts.roughMetal != UNX_NONE && (P[3].y & 1) == 0)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
                const float2 rm = mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, s.uv, s.duvdx, s.duvdy).xy;
                roughness *= rm.x;
                metallic *= rm.y;
            }

            // Shading normal (INTERFACES 8.1: TBN = (tangent, sign cross(n, t), normal) of the interpolants, result
            // normalised) and the footprint's slope variance trace.
            float variance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0;
            float3 n;
            if (ts.moments != UNX_NONE && (P[3].y & 2) == 0)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
                const MSlopeMoments mm = mNormalMoments(t, s.uv, s.duvdx, s.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
                const float3 B = s.tangentSign * cross(s.normal, s.tangent);
                n = normalize(s.tangent * mm.mean.x + B * mm.mean.y + s.normal);
                variance += mm.variance;
            }
            else n = normalize(s.normal);
            if (!s.front && (m.classFlags & MATERIAL_TWO_SIDED) != 0) n = -n;

            const float alpha = max(roughness * roughness, 1e-4);
            const float alphaFiltered = sqrt(alpha * alpha + variance);
            GBufferSample g;
            g.normal = n;
            g.baseColor = baseColor;
            g.roughness = min(sqrt(alphaFiltered), 1.0);
            const uint2 packed = encodeGBuffer(g);
            gbuffer[pixel] = packed;
            words[pixel] = mPackMaterialWord(s.material, metallic);

            if (ts.emissive != UNX_NONE && P[1].x != UNX_NONE)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.emissive];
                RWTexture2D<float4> emissive = ResourceDescriptorHeap[P[1].x];
                emissive[pixel] = float4(m.emissive * mSampleGrad(t, (ts.flags & M_TEX_EMISSIVE) != 0, s.uv, s.duvdx, s.duvdy).rgb, 1);
            }

            classBit = 1u << mShadeClass(materialClass(m));
            // R classifies with the stored (quantised) values; the tile minimum uses the same ones.
            const GBufferSample q = decodeGBuffer(packed);
            lobe = reflectionLobeHalfAngle(q.roughness, dot(q.normal, s.view)) / M_PI;

#if DEBUG
            RWStructuredBuffer<float4> dbg = ResourceDescriptorHeap[P[3].x];
            const uint di = 3 * (pixel.y * g_viewWidth + pixel.x);
            dbg[di] = float4(s.uv, s.duvdx);
            dbg[di + 1] = float4(s.duvdy, variance, g.roughness);
            dbg[di + 2] = float4(s.offset, s.front ? 1 : 0);
#endif
        }
    }

#if PLANAR_MASK
    if (P[3].z == UNX_NONE && P[3].w != UNX_NONE && all(pixel < uint2(g_viewWidth, g_viewHeight)))
    {
        Texture2D<uint> pixelMask = ResourceDescriptorHeap[P[3].w];
        if (WaveActiveAnyTrue(pixelMask[pixel] != 0) && WaveIsFirstLane()) InterlockedOr(gs_mirror, 1u);
    }
#endif
    const uint waveMask = WaveActiveBitOr(classBit);
    const float waveMin = WaveActiveMin(lobe);
    if (WaveIsFirstLane())
    {
        InterlockedOr(gs_classMask, waveMask);
        InterlockedMin(gs_minLobe, asuint(waveMin));  // non-negative floats order like their bits
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        RWTexture2D<unorm float> lobeTiles = ResourceDescriptorHeap[P[1].y];
        lobeTiles[gid] = floor(saturate(asfloat(gs_minLobe)) * 255.0) / 255.0;  // rounded down: never above the minimum
        RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].z];
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].w];
#if PLANAR_MASK
        const uint mask = gs_mirror != 0 ? gs_classMask : 0u;
#else
        const uint mask = gs_classMask;
#endif
        // The tile's band: the last band whose first row is at or above the tile's.
        const uint bands = P[4].x, height = P[4].y;
        uint band = 0, bandRow = 0;
        for (uint b = 1; b < bands; ++b)
        {
            const uint row = min(height, (height * b / bands) & ~7u);
            if (row <= gid.y * 8)
            {
                band = b;
                bandRow = row;
            }
        }
        [unroll] for (uint c = 0; c < M_CLASS_COUNT; ++c)
        {
            if ((mask & (1u << c)) == 0) continue;
            uint slot;
            args.InterlockedAdd(12 * (c * bands + band), 1, slot);
            tiles.Store(4 * (c * P[2].w + P[2].y * (bandRow / 8) + slot), gid.x | (gid.y << 16));
        }
    }
}
