// unx-kernel: cs_6_6 main
// unx-variants: DEBUG=0,1
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

[numthreads(8, 8, 1)]
void main(uint2 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        gs_classMask = 0;
        gs_minLobe = asuint(1.0);
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
        else
        {
            const MSurface s = mSurfaceFromVis(visId, P[0].y, float2(pixel) + 0.5);
            const GpuMaterial m = loadMaterial(s.material);
            const MTextureSet ts = mLoadTextureSet(P[2].x, s.material);

            float3 baseColor = m.baseColor;
            if (ts.baseColor != UNX_NONE)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
                baseColor *= t.SampleGrad(g_anisoWrap, s.uv, s.duvdx, s.duvdy).rgb;
            }
            float roughness = m.roughness, metallic = m.metallic;
            if (ts.roughMetal != UNX_NONE)
            {
                Texture2D<float2> t = ResourceDescriptorHeap[ts.roughMetal];
                const float2 rm = t.SampleGrad(g_anisoWrap, s.uv, s.duvdx, s.duvdy);
                roughness *= rm.x;
                metallic *= rm.y;
            }

            // Shading normal (INTERFACES 8.1: TBN = (tangent, sign cross(n, t), normal) of the interpolants, result
            // normalised) and the footprint's slope variance trace.
            float variance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0;
            float3 n;
            if (ts.moments != UNX_NONE)
            {
                Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
                const MSlopeMoments mm = mNormalMoments(t, s.uv, s.duvdx, s.duvdy, ts.slopeRange);
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
                emissive[pixel] = float4(m.emissive * t.SampleGrad(g_anisoWrap, s.uv, s.duvdx, s.duvdy).rgb, 1);
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
        const uint mask = gs_classMask;
        [unroll] for (uint c = 0; c < M_CLASS_COUNT; ++c)
        {
            if ((mask & (1u << c)) == 0) continue;
            uint slot;
            args.InterlockedAdd(12 * c, 1, slot);
            tiles.Store(4 * (c * P[2].w + slot), gid.x | (gid.y << 16));
        }
    }
}
