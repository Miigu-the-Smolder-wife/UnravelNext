// unx-kernel: cs_6_6 main
// unx-variants: DEBUG=0,1 PLANAR_MASK=0,1
// Material resolve (ARCHITECTURE 2.2), one 8 x 8 tile per group:
//   vis id -> surface (MaterialSurface.hlsli: exact pixel-centre barycentrics and their screen derivatives)
//   -> footprint-filtered textures (SampleGrad, anisotropic 16x) -> shading normal and band-limited roughness
//   -> G-buffer (GBuffer.hlsli), material word (MaterialInternal.hlsli), emissive (in frames with an emissive texture or
//      an emissive decal: every surface pixel's emission - the material's x its texture and mask, + the decals'),
//      tile class lists for the shading kernels, reflection lobe tiles for R (INTERFACES 5.1, v1.3).
// Specular band limit: the lobe is widened by the slope variance of the pixel footprint (Beckmann-equivalent
// alpha'^2 = alpha^2 + trace of the slope covariance):
//   - normal map: LEAN slope moments filtered by the hardware over the footprint (mNormalMoments);
//   - geometry: the interpolated normal varies linearly over the pixel box, trace = (|dn/dx|^2 + |dn/dy|^2) / 12.
// P[0] = { visId SRV, visibleClusters SRV, gbuffer UAV, material word UAV }
// P[1] = { emissive UAV or UNX_NONE, lobe tiles UAV, tile lists UAV (raw), tile args UAV (raw) }
// P[2] = { texture table SRV, tilesX, tilesY, tileCount }
// P[4] = { screen bands, view height, decal frames SRV, decal tiles SRV }: the class tile lists are split by band (MaterialSystem.h ResolveOutputs):
//        class c, band b at entry c * tileCount + tilesX * (row(b) / 8), arguments at 12 (c * bands + b), where row(b) is
//        RenderGraph::addBandedGroup's split min(H, floor(H b / bands) & ~7).
//        Decals (A7, E's Passes/Decal/Decal.hlsli; UNX_NONE = none this frame): decalApply modifies the pixel material
//        after the normal map and before the band limit (FEATURES_GAME 5.2), on the side the shading normal faces.
// P[5] = { E's surface state constants, table, pool (raw SRVs; UNX_NONE = no field), S's weather record SRV (UNX_NONE =
//        none) }: the surface state layers over the decals (SurfaceLayers.hlsli).
// P[6].x A9 anisotropy frame word UAV (R32_UINT; UNX_NONE = no anisotropic and no eye material in the scene): Aniso.hlsli's
//        word for anisotropic pixels, whose G-buffer roughness is then sqrt(sqrt(alpha_t' alpha_b')) (MATERIAL_LAYERS 1.5);
//        for an eye's pixels (MATERIAL_EYE, MaterialEye.hlsli) the eye word - the iris plane's normal, the iris mask and
//        the caustic weight; P[6].y = shading.eye_model (0: an eye's pixels take the word 0 and the surface's own uv);
//        for the pixels of a material with a height map (and neither of the above) the sun's visibility through the
//        height field in bits 0..7 (255 without material.parallax_shadow)
// P[6].z material inputs (MaterialInputs.hlsli): bits 0..7 material.parallax_steps (0: no parallax), bit 8
//        material.parallax_shadow
// P[3].y experiment mask (material.experiment_disable: cost attribution only, 0 otherwise)
// PLANAR_MASK=1 (planar reflection views with R's mask; views without one compile none of it):
// P[3].z R's planar tile mask (R8_UINT per 8 x 8 tile, nonzero = mirror pixels; UNX_NONE = absent), P[3].w R's planar
//        pixel mask (R8_UINT, read when there is no tile mask; UNX_NONE = absent). A planar reflection view's tile without
//        mirror pixels (V left them VIS_NONE) goes to no class list, so no shading kernel runs there (v1.22); its pixels
//        still get the sky word the neighbours' edge detection reads.
// P[3].x (DEBUG=1) RWStructuredBuffer<float4>, 3 per pixel: (uv, duv/dx), (duv/dy, variance, roughness'),
//        (camera-relative hit, front)
#define UNX_CLUSTER_STREAM 1  // (a vis id's triangle from its cluster's stream when it is compressed: ClusterStream.hlsli)
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Material/MaterialCut.hlsli"
#include "Passes/Material/MaterialTerrain.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#include "Passes/Decal/Decal.hlsli"
#include "Passes/Material/SurfaceLayers.hlsli"
#include "Passes/Material/Aniso.hlsli"
#include "Passes/Material/MaterialEye.hlsli"
#include "Passes/Material/MaterialInputs.hlsli"

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
            if (P[1].x != UNX_NONE)
            {
                RWTexture2D<float4> emissive = ResourceDescriptorHeap[P[1].x];
                emissive[pixel] = float4(loadMaterial(0).emissive, 1);
            }
            classBit = 1u << M_CLASS_OPAQUE;
            lobe = 0.5;
        }
        else
        {
            // (the triangle's deformed vertices stay at hand: an eye's frame takes them)
            const MVertex v0 = mTriangleVertex(visId, P[0].y, 0), v1 = mTriangleVertex(visId, P[0].y, 1), v2 = mTriangleVertex(visId, P[0].y, 2);
            const MSurface s = mSurfaceFromVertices(mTriangleIdentity(visId, P[0].y), v0, v1, v2, float2(pixel) + 0.5);
            const GpuMaterial m = loadMaterial(s.material);
            const MTextureSet ts = mLoadTextureSet(P[2].x, s.material);

            float3 baseColor, n;
            float roughness, metallic, variance;
            float occlusion = 1;  // the baked occlusion map's value (1: none)
            bool eye = false;  // an eye's pixel (MaterialEye.hlsli) and its eye word
            uint eyeWord = 0;
            // the material's uv and footprint (MaterialInputs.hlsli; the mesh's without a record), its parallax
            MInputUv iu = mInputUv(m, s.uv, s.duvdx, s.duvdy);
            bool parallax = false;
            float parallaxSun = 1;
            if (materialClass(m) == MATERIAL_CUT)
            {
                // A11 cut faces: textures through three object-space projections, the edge damage band (MaterialCut.hlsli)
                const MCutMaterial cm = mCutEvaluate(mCutFrame(visId, P[0].y, s), s, m, ts, P[3].y);
                baseColor = cm.baseColor, roughness = cm.roughness, metallic = cm.metallic, n = cm.normal;
                variance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0 + cm.variance;
            }
            else if (materialClass(m) == MATERIAL_TERRAIN)
            {
                // C5 terrain: up to 8 splat-weighted Standard layers (MaterialTerrain.hlsli)
                const MTerrainMaterial tm = mTerrainEvaluate(s, m, P[2].x, P[3].y);
                baseColor = tm.baseColor, roughness = tm.roughness, metallic = tm.metallic, n = tm.normal;
                variance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0 + tm.variance;
            }
            else
            {
                // Material inputs (MaterialInputs.hlsli): the streams the material reads - the second uv set, the vertex
                // colour -, then the parallax through its height field: every texture on uv set 0 moves with it (the
                // detail maps' set 0 by the same step, taken back through the uv transform).
                MVertexStreams streams;
                streams.uv1 = s.uv, streams.duv1dx = s.duvdx, streams.duv1dy = s.duvdy;
                streams.color = 1;
                if ((iu.r.flags & (MATERIAL_INPUT_OCCLUSION_UV1 | MATERIAL_INPUT_DETAIL_UV1 | MATERIAL_INPUT_VERTEX_TINT | MATERIAL_INPUT_VERTEX_BLEND)) != 0)
                    streams = mVertexStreams(visId, P[0].y, s);
                float2 uvDetail = s.uv;
                if (iu.r.heightTexture != UNX_NONE && (m.classFlags & MATERIAL_EYE) == 0 && (P[3].y & 1) == 0)
                {
                    parallax = true;
                    const float2 before = iu.uv;
                    mParallax(iu.r, s, iu.uv, iu.duvdx, iu.duvdy, P[6].z & 0xFFu, (P[6].z & 0x100u) != 0, parallaxSun);
                    const float2 moved = iu.uv - before;
                    const float det = iu.r.uvU.x * iu.r.uvV.y - iu.r.uvU.y * iu.r.uvV.x;
                    uvDetail += float2(iu.r.uvV.y * moved.x - iu.r.uvU.y * moved.y, iu.r.uvU.x * moved.y - iu.r.uvV.x * moved.x) / det;
                }

                roughness = m.roughness, metallic = m.metallic;
                if (ts.roughMetal != UNX_NONE && (P[3].y & 1) == 0)
                {
                    Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
                    const float2 rm = mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, iu.uv, iu.duvdx, iu.duvdy).xy;
                    roughness *= rm.x;
                    metallic *= rm.y;
                }
                // the baked occlusion map (materials without a layer record: the word's top byte is theirs), on the
                // material's uv or on the second set
                if (ts.occlusion != UNX_NONE && (P[3].y & 1) == 0 && (m.classFlags & MATERIAL_LAYERED) == 0)
                {
                    Texture2D<float4> t = ResourceDescriptorHeap[ts.occlusion];
                    const bool set1 = (iu.r.flags & MATERIAL_INPUT_OCCLUSION_UV1) != 0;
                    occlusion = mSampleGrad(t, (ts.flags & M_TEX_OCCLUSION) != 0, set1 ? streams.uv1 : iu.uv, set1 ? streams.duv1dx : iu.duvdx,
                                            set1 ? streams.duv1dy : iu.duvdy).x;
                }

                // Shading normal (INTERFACES 8.1: TBN = (tangent, sign cross(n, t), normal) of the interpolants, result
                // normalised) and the footprint's slope variance trace; the detail normal's slopes add in their own
                // frame, its variance to the trace.
                variance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0;
                const float3 B = s.tangentSign * cross(s.normal, s.tangent);
                float3 nSum = s.normal;
                if (ts.moments != UNX_NONE && (P[3].y & 2) == 0)
                {
                    Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
                    const MSlopeMoments mm = mNormalMoments(t, iu.uv, iu.duvdx, iu.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
                    const float2 slope = mInputSlope(iu.r, mm.mean);
                    nSum = s.tangent * slope.x + B * slope.y + s.normal;
                    variance += mm.variance;
                }
                float3 detailColor = 1;
                if ((iu.r.detailColorTexture != UNX_NONE || iu.r.detailNormalTexture != UNX_NONE) && (P[3].y & 3) == 0)
                {
                    const MDetail detail = mDetail(iu.r, s, uvDetail, streams, s.tangent, B);
                    detailColor = detail.colorFactor;
                    nSum += detail.normalTerm;
                    variance += detail.variance;
                }
                n = normalize(nSum);

                // The base colour, at the material's uv; an eye's at the iris point seen through the cornea, under the
                // limbal ring (the footprint stays the surface's); times the detail colour and the vertex colour.
                baseColor = m.baseColor;
                float2 uvColor = iu.uv;
                if ((m.classFlags & MATERIAL_EYE) != 0)
                {
                    eye = true;
                    if (P[6].y != 0)
                    {
                        const MEye e = mEyeEvaluate(visId, P[0].y, s, m, n, v0, v1, v2);
                        eyeWord = e.word;
                        uvColor = iu.on ? materialInputsUv(iu.r, e.uv) : e.uv;
                        baseColor *= e.darkening;
                    }
                }
                if (ts.baseColor != UNX_NONE && (P[3].y & 1) == 0)
                {
                    Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
                    baseColor *= mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, uvColor, iu.duvdx, iu.duvdy).rgb;
                }
                baseColor *= detailColor;
                if ((iu.r.flags & MATERIAL_INPUT_VERTEX_TINT) != 0) baseColor *= streams.color.rgb;
            }
            const bool backSide = !s.front && (m.classFlags & MATERIAL_TWO_SIDED) != 0;
            if (backSide) n = -n;

            float3 decalEmission = 0;  // E's emissive decals (Decal.hlsli)
            if (P[4].z != UNX_NONE)
            {
                // decals as upper layers of the material (their geometric normal: the side this pixel shades)
                DecalMaterial dm;
                dm.baseColor = baseColor; dm.roughness = roughness; dm.metallic = metallic; dm.normal = n; dm.variance = variance;
                dm.emissive = 0;
                DecalSurface ds;
                ds.position = s.offset; ds.dpdx = s.dpdx; ds.dpdy = s.dpdy;
                ds.geometricNormal = backSide ? -s.geometricNormal : s.geometricNormal;
                ds.instance = s.instance;
                ds.geometricVariance = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0;
                DecalContext dc;
                dc.frames = P[4].z; dc.tiles = P[4].w; dc.materialTable = P[2].x;
                decalApply(dc, pixel, ds, dm);
                baseColor = dm.baseColor; roughness = dm.roughness; metallic = dm.metallic; n = dm.normal; variance = dm.variance;
                decalEmission = dm.emissive;
            }
            if (P[5].x != UNX_NONE || P[5].w != UNX_NONE)
            {
                SurfaceLayerInputs li;
                li.surfaceConstants = P[5].x; li.surfaceTable = P[5].y; li.surfacePool = P[5].z; li.weather = P[5].w;
                SurfaceLayerMaterial lm;
                lm.baseColor = baseColor; lm.roughness = roughness; lm.metallic = metallic; lm.normal = n; lm.variance = variance;
                surfaceLayersApply(li, g_cameraPosition + s.offset, backSide ? -s.geometricNormal : s.geometricNormal,
                                   (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0, lm);
                baseColor = lm.baseColor; roughness = lm.roughness; metallic = lm.metallic; n = lm.normal; variance = lm.variance;
            }
            // the reference's rules on the final shading normal (MaterialInternal.hlsli)
            n = mNormalOnGeometricSide(n, backSide ? -s.geometricNormal : s.geometricNormal);
            if (s.front || backSide) n = mNormalTowardsViewer(n, s.view);

            const float alpha = max(roughness * roughness, 1e-4);
            const float alphaFiltered = sqrt(alpha * alpha + variance);
            GBufferSample g;
            g.normal = n;
            g.baseColor = baseColor;
            g.roughness = min(sqrt(alphaFiltered), 1.0);
            uint2 packed = encodeGBuffer(g);
            if ((m.classFlags & MATERIAL_ANISOTROPIC) != 0 && P[6].x != UNX_NONE)
            {
                // A9 anisotropy (MATERIAL_LAYERS 1.5): the cooked tangent's frame about the final normal, each axis
                // band-limited by the footprint (the geometric part per axis, the map's and layers' trace on both), the
                // word measured in the basis of the normal as stored; the G-buffer keeps the equal-area isotropic lobe
                float3 t, b;
                anisoFrameOfSurface(s.tangent, s.tangentSign, s.normal, anisoRecordOf(m), n, t, b);
                const float geometric = (dot(s.dndx, s.dndx) + dot(s.dndy, s.dndy)) / 12.0;
                const float2 af = anisoBandLimit(anisoAlphas(roughness, anisoRecordOf(m).strength), t, b, s.dndx, s.dndy, max(variance - geometric, 0.0));
                g.roughness = min(sqrt(sqrt(af.x * af.y)), 1.0);
                packed = encodeGBuffer(g);
                RWTexture2D<uint> anisoWords = ResourceDescriptorHeap[P[6].x];
                anisoWords[pixel] = anisoPackWord(decodeGBuffer(packed).normal, t, af);
            }
            gbuffer[pixel] = packed;
            float coatRoughness = 1 - saturate(occlusion);  // (a material without a layer: 255 x (1 - occlusion), mWordOcclusion)
            if ((m.classFlags & MATERIAL_LAYERED) != 0)
            {
                // A9: the coat's (or the sheen's) roughness band-limited by the footprint like the base's (MATERIAL_LAYERS
                // 3.4, 1.4): one layer kind per material, so both use the word's bits 24..31
                const GpuMaterialLayers layers = loadMaterialLayers(m.classFlags >> 16);
                const float rc = (m.classFlags & MATERIAL_SHEEN) != 0 ? layers.sheenRoughness : layers.clearcoatRoughness;
                const float ac = max(rc * rc, 1e-4);
                coatRoughness = min(sqrt(sqrt(ac * ac + variance)), 1.0);
            }
            if (eye && P[6].x != UNX_NONE)
            {
                RWTexture2D<uint> eyeWords = ResourceDescriptorHeap[P[6].x];  // (an eye's pixel: its eye word)
                eyeWords[pixel] = eyeWord;
            }
            if (parallax && P[6].x != UNX_NONE && (m.classFlags & (MATERIAL_ANISOTROPIC | MATERIAL_EYE)) == 0)
            {
                // a height map's pixel: the sun's visibility through the height field (the class word is free there)
                RWTexture2D<uint> classWords = ResourceDescriptorHeap[P[6].x];
                classWords[pixel] = uint(round(saturate(parallaxSun) * 255.0));
            }
            words[pixel] = mPackMaterialWord(s.material, metallic, coatRoughness);

            if (P[1].x != UNX_NONE)
            {
                // the pixel's emission: the material's (which holds emissiveScale), x its texture x its mask at the
                // material's uv where it has them, + the emissive decals'. Every surface pixel: the shading kernels
                // read the texture in place of the material's constant.
                float3 e = m.emissive;
                if (mEmissivePerPixel(ts))
                {
                    if (ts.emissive != UNX_NONE)
                    {
                        Texture2D<float4> t = ResourceDescriptorHeap[ts.emissive];
                        e *= mSampleGrad(t, (ts.flags & M_TEX_EMISSIVE) != 0, iu.uv, iu.duvdx, iu.duvdy).rgb;
                    }
                    e *= mInputEmissiveMask(iu);
                }
                RWTexture2D<float4> emissive = ResourceDescriptorHeap[P[1].x];
                emissive[pixel] = float4(e + decalEmission, 1);
            }

            classBit = 1u << mShadeClassOf(m);
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
