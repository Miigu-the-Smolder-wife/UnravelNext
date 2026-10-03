// Coverage composite: shared definitions of its kernels (design COVERAGE_REDESIGN 4.5; INTERFACES 7.1 v1.39). Owner: M.
// Every dispatch's work is bounded by the kernel's structure (a group handles at most COV_BLOCK records, one pixel of at
// most COV_LIGHT fragments, one 1,024-pair run or one round of COV_ROUND fragments of a heavy pixel), so the frame's cost
// follows the data and spreads over the GPU whatever the distribution (two TDRs on forest scenes with a per-tile walk,
// 2026-09-26). Loops over data-dependent counts are capped by engine constants (pool capacity) and set
// COV_M_ERROR_ITERATION_LIMIT (INTERFACES 3.6) if a cap is ever reached.
// V's records come pixel-major per listed tile (INTERFACES 7.1 v1.41): a pixel's fragments are one range, in no order.
// Stages (ShadingSystem.cpp, all indirect, sizes made on the GPU):
//   CoverageBegin         reset the state words; the heavy passes' and each round's arguments
//   CoverageComposite     per listed tile: light pixels (<= COV_LIGHT fragments) sorted in registers and composited;
//                         heavy pixels recorded
//   CoverageHeavySort     per (heavy pixel, run of COV_BLOCK records): (depth key, element) pairs sorted in groupshared
//                         into M's pair buffer (indexed like V's records)
//   CoverageHeavyRound    one wave per heavy pixel, a fixed number of dispatches: the next COV_ROUND fragments merged
//                         from the runs, shaded in parallel, weighted by the mask-union prefix, accumulated
//   CoverageHeavyFinish   per heavy pixel: the band A remainder, the output; unfinished = error bit
// Kernels that shade fragments share the constants P[1] = { visible clusters, M texture table, band A depth, colour UAV },
// P[3] = { froxel lights or UNX_NONE, LTC table, experiment mask, S fragment visibility or UNX_NONE }, P[4] = { atmosphere
// transmittance, multi-scatter, air volume, R's screen probes }, P[5].x = R's screen probe maps (UNX_NONE = absent),
// P[6].zw = { R's GI cache - or, without screen probes (gi.lumen_only), the indirect light's source word (GiSource.hlsli:
// the Lumen translucency volume, read at the fragment) -, S's per-record sun bytes (shadowFragmentSun) }, P[7].x = V's coverageDepthRange,
// P[7].zw = { E's surface state field (one raw SRV), S's weather record } (UNX_NONE: none): the surface layers
// (Passes/Material/SurfaceLayers.hlsli), as in the resolve. P[9] = W's sun-space water map (v1.77: depth, normal, medium,
// constants; UNX_NONE: no water) - a fragment under water from the sun takes the refracted sun and the water's
// transmittance (waterSunLight, the direct parts only); P[10].x its caustics (UNX_NONE: none). P[8].x = E's light function table (A8; UNX_NONE: none),
// P[8].y = ViewResources::coverageRecordRadiance (raw SRV, v1.75): special records (vis id top bits != 00: hair, streams,
// M pre-shaded classes) are read from it (their owners shaded them, CoverageSpecial.hlsli), clusters are shaded here.
// P[11].z = E's grooms between the fragments and the sun (Passes/Hair/HairShadow.hlsl MODE 2: the hair's transmittance
// at the 4 depths of S's sun profile, R32_UINT; UNX_NONE: none) - a cluster fragment's sun visibility times its value.
// P[10].zw = E's decals of the view (ViewResources::decalFrames, decalTiles; UNX_NONE: none live): a fragment's material
// takes them as the resolve's pixels do (Passes/Decal/Decal.hlsli decalApply), before the surface layers.
// P[8].w = material.parallax_steps (0: no parallax on fragments; covFragmentMaterial).
// COV_PRESHADE_CLASSES (CoverageSpecial.hlsl MODE=1, 2): covFragmentMaterial takes the material of its class as the
// resolve - 1 Cut, 2 Terrain; COV_PRESHADE_LIGHT (MODE=3): covShadeFragment lights the material those kernels stored.
#ifndef UNX_M_COVERAGE_SHADE_HLSLI
#define UNX_M_COVERAGE_SHADE_HLSLI
// L2c (14.1c): the listed coverage tile of the records being shaded (CoverageComposite sets it; the special and heavy
// kernels leave it unset: their records keep the per-record loop). P[5].w = the field (raw SRV, UNX_NONE: off),
// P[10].y = the field's capacity in tiles.
static uint g_covListed = 0xFFFFFFFFu;
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Material/MaterialEye.hlsli"
#include "Passes/Material/MaterialInputs.hlsli"
#include "Passes/Decal/Decal.hlsli"
#include "Passes/Material/SurfaceLayers.hlsli"
#include "Passes/Lights/LightFunction.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Lights/CoverageTileLights.hlsli"  // L2c (14.1c): the tile x depth-interval FAR field (P[5].w, P[10].y)
#include "Passes/Water/WaterLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shading/CoverageSpecial.hlsli"
#include "Passes/Shading/MegaLights.hlsli"  // (mlDiffuseFactor, mlSpecularFactor: the coverage instance, P[11].xy)
#if COV_PRESHADE_CLASSES == 1
#include "Passes/Material/MaterialCut.hlsli"
#elif COV_PRESHADE_CLASSES == 2
#include "Passes/Material/MaterialTerrain.hlsli"
#endif
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Visibility/Coverage.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"
#include "PrimitiveOrder.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#define GI_PROBE_TILE_CACHE
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiSource.hlsli"

#define COV_LIGHT 16u     // fragments of a light pixel (sorted in registers)
#define COV_ROUND 32u     // fragments a heavy pixel's round takes
#define COV_M_ERROR_ITERATION_LIMIT 0x400u
#define COV_M_ERROR_ROUNDS 0x800u  // a heavy pixel still open after the frame's rounds (more next frame)

// State words (raw buffer 'm.coverage.state').
#define COVS_HEAVY 2u       // heavy pixels
#define COVS_ERRORS 3u      // error bits (copied to M's statistics)
#define COVS_RUNS 4u        // runs of all heavy pixels (cursor slots)
#define COVS_MAX_RUNS 5u    // the most runs of one heavy pixel
#define COVS_OPEN 6u        // 6..7 open heavy pixels appended to active list 0 / 1 by a round
#define COVS_ENTRIES 8u     // compact form: entries of the visible fragment list (CoverageWalk)
#define COVS_WALKED 9u      // statistics: records the light pixels' walks visited (nearer first, until the pixel is complete)
#define COVS_SHADED 10u     // statistics: of them, the records with weight (shaded)
#define COVS_LIGHT_PIXELS 11u  // statistics: light pixels
#define COVS_WORDS 16u
// Compact form of the light pixels' composite (shading.coverage_compact; CoverageWalk -> CoverageShadeList x 2 ->
// CoverageGather). The composite kernel shades inside each pixel lane's own front-to-back loop, so a wave runs the
// shading as many times as its deepest pixel has visible fragments, with the other lanes idle. Here the walk only
// weights: a tile's fragments with weight become one contiguous run of entries { record element, weight } in pixel
// order, the shading kernels take one entry per lane (64 at a time, the tile's probes and light field as before) and
// keep weight x radiance per entry, and the gather sums each pixel's entries with the band A remainder. The same
// weights and the same covShadeFragment calls as the composite; the sums are in float (the composite keeps part 1's sum
// in half floats between its two kernels).
//   m.coverage visible     raw, 8 B per entry: { element, weight (float bits) }
//   m.coverage radiance    raw, 12 B per entry: weight x exposed radiance (float3), part 1 then part 1 + part 2
//   m.coverage tile spans  raw, 8 B per listed tile: { first entry, entries }
//   m.coverage pixel spans raw, 8 B per pixel of a listed tile (listed x 64 + pixel): { the pixel's first entry inside
//                          the tile's run (bits 0..15) | entries << 16 | kind << 24, weight sum (float bits) }
#define COVC_TILE_ENTRIES (COV_TILE_PIXELS * COV_LIGHT)  // entries of one tile at most (1,024: the shading rounds' bound)
#define COVC_NONE 0u   // no record, or outside the view
#define COVC_LIGHT 1u  // composited from its entries (CoverageGather writes the pixel)
#define COVC_HEAVY 2u  // a heavy pixel (CoverageHeavy* write it)
// Dispatch arguments (raw buffer 'm.coverage.args', written by CoverageBegin, read as arguments).
#define COVA_SORT 4u        // 4..6 (heavy pixels x, y, most runs of one)
#define COVA_ROUND 8u       // 8..10 the next heavy round (open pixels)
#define COVA_FINISH 12u     // 12..14 heavy pixels / 64
#define COVA_WORDS 16u
#define COV_ROUNDS 8u       // heavy rounds per frame: up to 256 fragments composited per pixel (design 4.5: more is a defect)
// Heavy pixel record (raw buffer 'm.coverage.heavy', COVH_WORDS words each).
#define COVH_PIXEL 0u       // x | y << 16
#define COVH_SEGMENT 1u     // first pair
#define COVH_COUNT 2u       // fragments
#define COVH_RUNS 3u        // runs of COV_BLOCK pairs
#define COVH_CURSORS 4u     // first cursor slot (one per run)
#define COVH_COVERED 5u     // mask union so far
#define COVH_USED 6u        // sum of weights so far (float bits)
#define COVH_DONE 7u        // 1: complete (union full, weights at 1, fragments exhausted or behind band A)
#define COVH_SUM 8u         // 8..10 exposed radiance so far (float bits)
#define COVH_LAST 11u       // 11 the last fragment's depth key, 12 its vis id (clip duplicate check)
#define COVH_WORDS 16u

// Band A radiance under a pixel's fragments: what the shading kernels kept for coverage tiles, or the E composite's
// resolved sum on edge pixels. P[2] = { band A radiance SRV, resolved SRV or UNX_NONE, edge tile mask SRV or UNX_NONE, .. }.
float3 covBandA(uint2 pixel)
{
    Texture2D<float4> bandRadiance = ResourceDescriptorHeap[P[2].x];
    if (P[2].y != UNX_NONE && P[2].z != UNX_NONE)
    {
        Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[2].z];
        const uint2 edgeMask = edgeTiles[pixel / COV_TILE_PX];
        const uint bit = (pixel.x % COV_TILE_PX) + COV_TILE_PX * (pixel.y % COV_TILE_PX);
        if ((((bit < 32 ? edgeMask.x : edgeMask.y) >> (bit & 31)) & 1u) != 0)
        {
            Texture2D<float4> resolved = ResourceDescriptorHeap[P[2].y];
            return resolved[pixel].rgb;
        }
    }
    return bandRadiance[pixel].rgb;
}

// R's screen probes of the group's tile into the tile cache (lanes 0..24; the caller syncs the group).
uint4 covProbeFetch(uint2 tileCoord, uint lane)
{
    if (P[4].w == UNX_NONE || (P[3].z & 6) == 6) return 0;
    ProbeSrvs probes;
    probes.probes = P[4].w;
    probes.occlusion = P[4].w;
    probes.pad0 = P[5].x;
    probes.pad1 = P[6].z != UNX_NONE ? P[6].z + 1 : 0;  // R's GI cache
    return giProbeTileFetch(probes, tileCoord, lane, giProbeCountOfView());
}

// Nearer first; at equal depth use stable geometry, then the fragment payload.
// Only byte-identical fragments may fall back to their temporary pool element.
bool covBefore(uint2 a, uint2 b, StructuredBuffer<uint4> records, uint visibleSrv)
{
    if (a.x != b.x) return a.x > b.x;
    if (a.y == b.y) return false;
    if (a.y == 0xFFFFFFFFu || b.y == 0xFFFFFFFFu) return a.y < b.y;
    const uint4 ra = records[a.y], rb = records[b.y];
    const uint ka = ra.x >> 30, kb = rb.x >> 30;
    if (ka != kb) return ka < kb;
    const uint64_t ia = ka < 2 ? primitiveOrder(visibleSrv, coverageClusterVisId(ra.x)) : (uint64_t)ra.x;
    const uint64_t ib = kb < 2 ? primitiveOrder(visibleSrv, coverageClusterVisId(rb.x)) : (uint64_t)rb.x;
    if (ia != ib) return ia < ib;
    if (ra.y != rb.y) return ra.y < rb.y;
    if (ra.z != rb.z) return ra.z < rb.z;
    if (ra.w != rb.w) return ra.w < rb.w;
    return a.y < b.y;
}
uint2 covKey(StructuredBuffer<uint4> records, uint element) { return uint2(records[element].y & ~COV_DEPTH_SEE_THROUGH, element); }
// covBefore in two steps, for a kernel that sorts in registers with an unrolled network (CoverageComposite): the network
// compares by covNearer - the depth, then the element: a total order, but not yet the stable one among equal depths -
// and COV_SETTLE_TIES then puts each run of equal depth into covBefore's order by insertion, on the lane's stored keys
// (sorted[0 .. count)). The result is covBefore's order (the order is total, so it is the only one), and covBefore
// stands once in the kernel: in each of the network's 80 comparators it was 9,000 instructions, 40 KB of a kernel at the
// size limit. Equal depths are rare, so the insertion costs one comparison of depths per key.
bool covNearer(uint2 a, uint2 b) { return a.x != b.x ? a.x > b.x : a.y < b.y; }
#define COV_SETTLE_TIES(sorted, count, records, visibleSrv)                                         \
    [loop] for (uint tieI = 1; tieI < (count); ++tieI)                                              \
    {                                                                                               \
        const uint2 tieKey = sorted[tieI];                                                          \
        uint tieJ = tieI;                                                                           \
        [loop] while (tieJ > 0)                                                                     \
        {                                                                                           \
            const uint2 tiePrev = sorted[tieJ - 1];                                                 \
            if (tiePrev.x != tieKey.x || !covBefore(tieKey, tiePrev, records, visibleSrv)) break;   \
            sorted[tieJ] = tiePrev;                                                                 \
            --tieJ;                                                                                 \
        }                                                                                           \
        if (tieJ != tieI) sorted[tieJ] = tieKey;                                                    \
    }
static const uint2 COV_KEY_AFTER_ALL = uint2(0, 0xFFFFFFFFu);  // after every key (a record's element is below it)


// Pixel coordinates of a camera-relative point (x right, y down), w = view depth (EdgeComposite.hlsl edgeProject).
float3 covProject(float3 offset)
{
    const float3 v = float3(dot(g_view[0].xyz, offset), dot(g_view[1].xyz, offset), dot(g_view[2].xyz, offset));
    const float4 clip = mul(g_proj, float4(v, 1));
    return float3((clip.x / clip.w * 0.5 + 0.5) * g_viewWidth, (0.5 - clip.y / clip.w * 0.5) * g_viewHeight, clip.w);
}


// S's shadowing of a fragment (INTERFACES 7.3 v1.41, shadowFragmentVisibility): the sun from the 4-point profile along the
// pixel's ray between its nearest and farthest record, linear in view depth, or the record's own byte where S flagged the
// pixel a pair (shadowFragmentSun, element 'element' of coverageRecords); local slots 1..3 as S measured them at the two
// ends (each end's froxel list order), interpolated the same way. valid = 0: S published nothing (all 1).
struct CovFragmentShadow
{
    float sun, t;
    uint nearSlots, farSlots;
    bool valid;
};
float covByte(uint word, uint b) { return ((word >> (8u * b)) & 255u) / 255.0; }
CovFragmentShadow covFragmentShadow(uint2 pixel, uint element, float linearZ)
{
    CovFragmentShadow o;
    o.sun = 1;
    o.t = 0;
    o.nearSlots = o.farSlots = 0xFFFFFFFFu;
    o.valid = P[3].w != UNX_NONE && P[7].x != UNX_NONE;
    if (!o.valid) return o;
    StructuredBuffer<uint3> visibility = ResourceDescriptorHeap[P[3].w];
    Texture2D<uint2> range = ResourceDescriptorHeap[P[7].x];
    const uint3 w = visibility[pixel.y * g_viewWidth + pixel.x];
    const uint2 r = range[pixel];  // device depth bits: nearest (larger) and farthest
    const float zNear = linearDepth(asfloat(r.x)), zFar = linearDepth(asfloat(r.y));
    o.t = zFar > zNear ? saturate((linearZ - zNear) / (zFar - zNear)) : 0;
    if ((w.y & 1u) != 0 && P[6].w != UNX_NONE)
    {
        ByteAddressBuffer sunBytes = ResourceDescriptorHeap[P[6].w];
        o.sun = covByte(sunBytes.Load((element >> 2) * 4u), element & 3u);
    }
    else
    {
        const float x = o.t * 3;
        const uint k = min((uint)x, 2u);
        o.sun = lerp(covByte(w.x, k), covByte(w.x, k + 1u), x - k);
    }
    if (P[11].z != UNX_NONE)
    {
        // the grooms towards the sun at the fragment's depth (the same 4 points)
        Texture2D<uint> hairSun = ResourceDescriptorHeap[P[11].z];
        const uint h = hairSun[pixel];
        const float x = o.t * 3;
        const uint k = min((uint)x, 2u);
        o.sun *= lerp(covByte(h, k), covByte(h, k + 1u), x - k);
    }
    o.nearSlots = w.y;
    o.farSlots = w.z;
    return o;
}
// A local light's visibility: its slot in the froxel list at each end (S's shadowSlotOfLight at z_near and z_far); a light
// past the third shadowed one of a list has no fragment slot there and counts as visible (INTERFACES 7.3 v1.41 carries
// no overflow for fragments).
// nearCasters / farCasters: shadowFirstCasters at z_near and z_far, once per fragment (two list walks per light before).
float covLocalVisibility(CovFragmentShadow s, uint3 nearCasters, uint3 farCasters, uint lightIndex)
{
    if (!s.valid) return 1;
    const uint a = shadowSlotAmong(nearCasters, lightIndex), b = shadowSlotAmong(farCasters, lightIndex);
    const float vn = a >= 1 && a <= 3 ? covByte(s.nearSlots, a) : 1;
    const float vf = b >= 1 && b <= 3 ? covByte(s.farSlots, b) : 1;
    return lerp(vn, vf, s.t);
}

// Exposed linear radiance of one fragment (the band A kernel's lighting for a surface point: ShadeOpaque.hlsl); 'element'
// is its record's element in coverageRecords (S's per-record sun byte).
// The covered region's centroid in the pixel, where V evaluated the fragment's depth (the pixel centre when a vertex lies
// behind the eye).
float2 covFragmentCentre(MVertex v0, MVertex v1, MVertex v2, uint2 pixel)
{
    const float3 a = covProject(v0.world - g_cameraPosition), b = covProject(v1.world - g_cameraPosition), c = covProject(v2.world - g_cameraPosition);
    float2 centre = float2(pixel) + 0.5;
    if (min(a.z, min(b.z, c.z)) > 0) coverageTriangleAreaCentroid(a.xy, b.xy, c.xy, float2(pixel), centre);
    return centre;
}

// A fragment's material at its footprint (Resolve.hlsl): Standard textures, or with COV_PRESHADE_CLASSES the class's.
struct CovMaterial
{
    float3 baseColor;
    float roughness, metallic;
    float3 normal;    // world, unit (before the side and view rules)
    float variance;   // slope variance (geometric + textures)
    float coatRoughness;  // A9: the coat's footprint-filtered perceptual roughness (layered materials; 0 otherwise)
    uint eyeWord;
};
// v0..v2: the triangle's deformed vertices (the ones sf was made from). An eye's fragment (MATERIAL_EYE) reads its base
// colour at the iris point seen through the cornea, under the limbal ring (MaterialEye.hlsli); it is shaded as the plain
// Subsurface model (a fragment has no eye word). Material inputs (MaterialInputs.hlsli), as the resolve applies them: the
// uv transform, the second uv set and the vertex colour (the streams of the fragment's triangle), the parallax through
// the height field - 'parallax': material.parallax_steps in bits 0..7, 0 none; a fragment has no class word, so the
// field's own shadow of the sun is the resolve's alone -, the detail colour and normal (with the normal's variance in
// the footprint's) and the vertex tint. The occlusion map is not read (a fragment's indirect light takes none).
CovMaterial covFragmentMaterial(uint visId, MSurface sf, GpuMaterial m, MTextureSet ts, MVertex v0, MVertex v1, MVertex v2, uint parallax)
{
    float3 baseColor = m.baseColor, n;
    uint eyeWord = 0;
    float roughness = m.roughness, metallic = m.metallic;
    float variance = (dot(sf.dndx, sf.dndx) + dot(sf.dndy, sf.dndy)) / 12.0;
#if COV_PRESHADE_CLASSES == 1
    if (materialClass(m) == MATERIAL_CUT)
    {
        // A11 cut faces (MaterialCut.hlsli), as the resolve (no material experiments here)
        const MCutMaterial cm = mCutEvaluate(mCutFrame(visId, P[1].x, sf), sf, m, ts, 0);
        baseColor = cm.baseColor, roughness = cm.roughness, metallic = cm.metallic, n = cm.normal;
        variance += cm.variance;
    }
    else
#elif COV_PRESHADE_CLASSES == 2
    if (materialClass(m) == MATERIAL_TERRAIN)
    {
        // C5 terrain layers (MaterialTerrain.hlsli), as the resolve
        const MTerrainMaterial tm = mTerrainEvaluate(sf, m, P[1].y, 0);
        baseColor = tm.baseColor, roughness = tm.roughness, metallic = tm.metallic, n = tm.normal;
        variance += tm.variance;
    }
    else
#endif
    {
        MInputUv iu = mInputUv(m, sf.uv, sf.duvdx, sf.duvdy);
        // the streams the material reads, then the parallax: every texture on uv set 0 moves with it, the detail maps'
        // set 0 by the same step taken back through the uv transform (Resolve.hlsl)
        MVertexStreams streams;
        streams.uv1 = sf.uv, streams.duv1dx = sf.duvdx, streams.duv1dy = sf.duvdy;
        streams.color = 1;
        if ((iu.r.flags & (MATERIAL_INPUT_DETAIL_UV1 | MATERIAL_INPUT_VERTEX_TINT | MATERIAL_INPUT_VERTEX_BLEND)) != 0) streams = mVertexStreams(visId, P[1].x, sf);
        float2 uvDetail = sf.uv;
        if (iu.r.heightTexture != UNX_NONE && (m.classFlags & MATERIAL_EYE) == 0 && (parallax & 0xFFu) != 0)
        {
            const float2 before = iu.uv;
            float unusedSun;
            mParallax(iu.r, sf, iu.uv, iu.duvdx, iu.duvdy, parallax & 0xFFu, false, unusedSun);
            const float2 moved = iu.uv - before;
            const float det = iu.r.uvU.x * iu.r.uvV.y - iu.r.uvU.y * iu.r.uvV.x;
            uvDetail += float2(iu.r.uvV.y * moved.x - iu.r.uvU.y * moved.y, iu.r.uvU.x * moved.y - iu.r.uvV.x * moved.x) / det;
        }
        if (ts.roughMetal != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
            const float2 rm = mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, iu.uv, iu.duvdx, iu.duvdy).xy;
            roughness *= rm.x;
            metallic *= rm.y;
        }
        const float3 B = sf.tangentSign * cross(sf.normal, sf.tangent);
        float3 nSum = sf.normal;
        if (ts.moments != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
            const MSlopeMoments mm = mNormalMoments(t, iu.uv, iu.duvdx, iu.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
            const float2 slope = mInputSlope(iu.r, mm.mean);
            nSum = sf.tangent * slope.x + B * slope.y + sf.normal;
            variance += mm.variance;
        }
        float3 detailColor = 1;
        if (iu.r.detailColorTexture != UNX_NONE || iu.r.detailNormalTexture != UNX_NONE)
        {
            const MDetail detail = mDetail(iu.r, sf, uvDetail, streams, sf.tangent, B);
            detailColor = detail.colorFactor;
            nSum += detail.normalTerm;
            variance += detail.variance;
        }
        n = normalize(nSum);
        float2 uvColor = iu.uv;
        if ((m.classFlags & MATERIAL_EYE) != 0)
        {
            const MEye e = mEyeEvaluate(visId, P[1].x, sf, m, n, v0, v1, v2);
            uvColor = iu.on ? materialInputsUv(iu.r, e.uv) : e.uv;
            baseColor *= e.darkening;
            eyeWord = e.word;
        }
        if (ts.baseColor != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
            baseColor *= mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, uvColor, iu.duvdx, iu.duvdy).rgb;
        }
        baseColor *= detailColor;
        if ((iu.r.flags & MATERIAL_INPUT_VERTEX_TINT) != 0) baseColor *= streams.color.rgb;
    }
    CovMaterial o;
    o.baseColor = baseColor;
    o.roughness = roughness;
    o.metallic = metallic;
    o.normal = n;
    o.variance = variance;
    o.coatRoughness = 0;
    o.eyeWord = eyeWord;
    return o;
}

// COV_PRESHADE_LIGHT (CoverageSpecial.hlsl MODE=3): the material a class kernel stored for this special entry
// (g_covPreshadeSlot; P[5].y raw SRV, 48 B per entry: base colour, roughness, normal, metallic, variance, coat roughness;
// f32). A9 coats (CoverageSpecial MODE 5 / 6): COV_COAT adds the coat terms as ShadeOpaque LAYERED; COV_PART 1 is the
// emission, sun and local lights before the air (unexposed, into M's scratch P[5].z), COV_PART 2 the indirect light plus
// that part, then the air and the exposure - two kernels, as one exceeds the 200 KB DXIL limit.
// COV_PART_EXPOSED (the composite and heavy rounds, split the same way for the DXIL limit): part 1 returns the emission,
// sun and local lights exposed and through the air's transmittance, part 2 the indirect light the same way plus the air's
// in-scattering - their sum is the one-kernel value; part 2 loads no earlier part (the caller sums).
#if COV_PRESHADE_LIGHT
static uint g_covPreshadeSlot;
#endif
void covStoreMaterial(RWByteAddressBuffer b, uint slot, CovMaterial c)
{
    b.Store4(48 * slot, uint4(asuint(c.baseColor), asuint(c.roughness)));
    b.Store4(48 * slot + 16, uint4(asuint(c.normal), asuint(c.metallic)));
    b.Store2(48 * slot + 32, uint2(asuint(c.variance), asuint(c.coatRoughness)));
    b.Store(48 * slot + 40, c.eyeWord);
}
CovMaterial covLoadMaterial(ByteAddressBuffer b, uint slot)
{
    const uint4 a = b.Load4(48 * slot), d = b.Load4(48 * slot + 16);
    CovMaterial c;
    c.baseColor = asfloat(a.xyz);
    c.roughness = asfloat(a.w);
    c.normal = asfloat(d.xyz);
    c.metallic = asfloat(d.w);
    const uint2 vr = b.Load2(48 * slot + 32);
    c.variance = asfloat(vr.x);
    c.coatRoughness = asfloat(vr.y);
    c.eyeWord = b.Load(48 * slot + 40);
    return c;
}

float3 covShadeFragment(uint visId, uint element, uint2 pixel, uint experiment)
{
    // The triangle and the covered region's centroid in this pixel, where V evaluated the fragment's depth.
    const MTriangleIdentity id = mTriangleIdentity(visId, P[1].x);
    const MVertex v0 = mTriangleVertex(visId, P[1].x, 0), v1 = mTriangleVertex(visId, P[1].x, 1), v2 = mTriangleVertex(visId, P[1].x, 2);
    const float2 centre = covFragmentCentre(v0, v1, v2, pixel);
    const MSurface sf = mSurfaceFromVertices(id, v0, v1, v2, centre);

    // Material at the footprint (Resolve.hlsl).
    const GpuMaterial m = loadMaterial(sf.material);
    const MTextureSet ts = mLoadTextureSet(P[1].y, sf.material);
#if COV_PRESHADE_LIGHT
    ByteAddressBuffer preshaded = ResourceDescriptorHeap[P[5].y];
    const CovMaterial cmat = covLoadMaterial(preshaded, g_covPreshadeSlot);
#else
    const CovMaterial cmat = covFragmentMaterial(visId, sf, m, ts, v0, v1, v2, P[8].w);  // P[8].w: material.parallax_steps
#endif
    float3 baseColor = cmat.baseColor, n = cmat.normal;
    float roughness = cmat.roughness, metallic = cmat.metallic, variance = cmat.variance;
    const bool backSide = !sf.front && (m.classFlags & MATERIAL_TWO_SIDED) != 0;
    if (backSide) n = -n;
    float3 decalEmission = 0;  // what E's emissive decals add to the fragment's emission (Decal.hlsli; the resolve's sum)
    if (P[10].z != UNX_NONE)
    {
        // E's decals of the view as upper layers of the fragment's material, as in the resolve and before the surface
        // layers (P[10].zw: the view's decal frames and tile lists; the tile is the fragment's pixel's, the footprint
        // the pixel's on the fragment's plane, the geometric normal the side this fragment shades)
        DecalMaterial dm;
        dm.baseColor = baseColor; dm.roughness = roughness; dm.metallic = metallic; dm.normal = n; dm.variance = variance;
        dm.emissive = 0;
        DecalSurface ds;
        ds.position = sf.offset; ds.dpdx = sf.dpdx; ds.dpdy = sf.dpdy;
        ds.geometricNormal = backSide ? -sf.geometricNormal : sf.geometricNormal;
        ds.instance = sf.instance;
        ds.geometricVariance = (dot(sf.dndx, sf.dndx) + dot(sf.dndy, sf.dndy)) / 12.0;
        DecalContext dc;
        dc.frames = P[10].z; dc.tiles = P[10].w; dc.materialTable = P[1].y;
        decalApply(dc, pixel, ds, dm);
        baseColor = dm.baseColor; roughness = dm.roughness; metallic = dm.metallic; n = dm.normal; variance = dm.variance;
        decalEmission = dm.emissive;
    }
    if (P[7].z != UNX_NONE || P[7].w != UNX_NONE)
    {
        // the surface state layers, as in the resolve (a grass blade or leaf gets wet, frost, snow like any surface)
        SurfaceLayerInputs li;
        li.surfaceConstants = P[7].z; li.surfaceTable = P[7].z; li.surfacePool = P[7].z; li.weather = P[7].w;
        SurfaceLayerMaterial lm;
        lm.baseColor = baseColor; lm.roughness = roughness; lm.metallic = metallic; lm.normal = n; lm.variance = variance;
        surfaceLayersApply(li, g_cameraPosition + sf.offset, backSide ? -sf.geometricNormal : sf.geometricNormal,
                           (dot(sf.dndx, sf.dndx) + dot(sf.dndy, sf.dndy)) / 12.0, lm);
        baseColor = lm.baseColor; roughness = lm.roughness; metallic = lm.metallic; n = lm.normal; variance = lm.variance;
    }
    // the reference's rules on the final shading normal (MaterialInternal.hlsli), as in the resolve
    n = mNormalOnGeometricSide(n, backSide ? -sf.geometricNormal : sf.geometricNormal);
    if (sf.front || backSide) n = mNormalTowardsViewer(n, sf.view);
    const float alphaIn = max(roughness * roughness, 1e-4);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = baseColor;
    s.roughness = min(sqrt(sqrt(alphaIn * alphaIn + variance)), 1.0);
    s.metallic = metallic;
    s.specular = m.specular;
    s.transmission = m.transmission;
    float3 radiance = m.emissive;
    if (mEmissivePerPixel(ts))
    {
        // (the emission x its texture x its mask at the material's uv, as the resolve)
        const MInputUv eu = mInputUv(m, sf.uv, sf.duvdx, sf.duvdy);
        if (ts.emissive != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.emissive];
            radiance *= mSampleGrad(t, (ts.flags & M_TEX_EMISSIVE) != 0, eu.uv, eu.duvdx, eu.duvdy).rgb;
        }
        radiance *= mInputEmissiveMask(eu);
    }
    radiance += decalEmission;
#if COV_PART == 2 && COV_PART_EXPOSED
    radiance = 0;  // (the emission is part 1's)
#elif COV_PART == 2
    {
        ByteAddressBuffer direct = ResourceDescriptorHeap[P[5].z];  // part 1: emission, sun, local lights
        radiance = asfloat(direct.Load3(16 * g_covPreshadeSlot));
    }
#endif

    float3 D, Dx, Dy;
    mPixelRay(centre, D, Dx, Dy);
    const float3 offset = sf.offset;
    const float3 worldPos = g_cameraPosition + offset;
    const float linearZ = dot(offset, -g_view[2].xyz);  // view depth (D has unit depth along the view axis)
    const float3 v = sf.view;
    const ModelEye eye = modelEyeOf(cmat.eyeWord, n);
    const float NoV = dot(n, v);
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);
#if COV_COAT
    const float3 f0 = modelFilmBegin(m, modelF0(s));  // A9 thin film (as ShadeOpaque LAYERED == 1)
#else
    const float3 f0 = modelF0(s);
#endif
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;
    // The Subsurface class's model, by class (as ShadeOpaque LAYERED=3; MaterialModel.hlsli ModelSubsurface): its two
    // specular lobes at the fragment's roughness and f_d x transmission for the light through thin parts (0: none). A
    // fragment of another class takes none of it.
    const bool subsurface = s.cls == MATERIAL_SUBSURFACE;
    ModelSubsurface skin = (ModelSubsurface)0;
    if (subsurface) skin = modelSubsurfaceOf(m, s.roughness);
    const float3 thin = subsurface ? diffuse * s.transmission : 0;
    const float lobeRoughness = subsurface ? skin.roughness : s.roughness;  // the roughness of the specular's compensation
    float keepCloth = 1;  // the cloth blend (COV_COAT: a sheen's cloth factor): what stays of the base's specular lobe
#if COV_COAT
    ModelCoat coat = modelCoatOf(m);
    coat.roughness = cmat.coatRoughness;
    const float cover = coat.cover, keep = 1 - cover;
    // A9 sheen (as ShadeOpaque LAYERED=2; MATERIAL_LAYERS 1.4): the layer record's colour (0 without a sheen), the stored
    // footprint-filtered roughness
    ModelSheen sheen = modelSheenOf(m);
    sheen.roughness = max(cmat.coatRoughness, 0.1);
    const bool sheenOn = max(sheen.color.r, max(sheen.color.g, sheen.color.b)) > 0 && NoV > 0;
    const float keepS = sheenOn ? modelSheenKeep(sheen, NoV) : 1;
    if (sheenOn) keepCloth = 1 - sheen.cloth;
#endif

    // ---- sun (with S's air at the fragment's depth: in-scatter and transmittance in front of it, the sun's illuminance)
    AtmosphereSrvs atm;
    atm.transmittance = P[4].x;
    atm.multiScatter = P[4].y;
    atm.skyView = UNX_NONE;
    atm.aerial = P[4].z;
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (atm.transmittance != UNX_NONE)
    {
        if (atm.aerial != UNX_NONE && (experiment & 8) == 0)
            atmosphereAirView(atm, centre / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    float3 l0 = normalize(g_sunDirection);
#if COV_PART != 2
    if (P[9].w != UNX_NONE)
    {
        // W stage 2 (v1.77), as ShadeOpaque: under water from the sun, the refracted direction and the water's transmittance
        float3 lw, tw;
        if (waterSunLight(P[9].x, P[9].y, P[9].z, P[9].w, P[10].x, worldPos, l0, 0, lw, tw))  // P[10].x: W's caustics
        {
            l0 = lw;
            E *= tw;
        }
    }
#endif
    const float NoL = dot(n, l0);
    const CovFragmentShadow shadow = covFragmentShadow(pixel, element, linearZ);
    const float sunVisibility = shadow.sun;
#if COV_PART != 2
    if (sunVisibility > 0 && (experiment & 16) == 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));
        const float above = shCapCosine(NoL), below = shCapCosine(-NoL);
        float3 sun = 0;
        if (NoV > 0)
        {
            if (above > 0)
            {
                const float e = modelDirectionalAlbedo(NoV, lobeRoughness);
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap;
                // the specular lobe - the Subsurface class's two, each at its own roughness and weighed by the mix -
                // through one inlined disk integral
                const uint lobes = subsurface && skin.mix < 1 ? 2 : 1;
                [loop] for (uint lobe = 0; lobe < lobes; ++lobe)
                {
                    const float lr = subsurface ? (lobe == 1 ? skin.roughness1 : skin.roughness0) : s.roughness;
                    const float3 spec = shSunSpecular(f0, lr, modelAlpha(lr), compensation, n, v, NoV, l0, E, shPixelAngle(D, Dx));
                    sun += (subsurface ? spec * (lobe == 1 ? 1 - skin.mix : skin.mix) : spec) * keepCloth;
                }
            }
            if (foliage) sun += back * below * cap;
            if (subsurface && s.transmission > 0 && below > 0) sun += thin * (modelSubsurfaceThin(below, v, l0) * cap);
        }
        else if (foliage) sun = back * above * cap;
        else if (subsurface && s.transmission > 0 && above > 0) sun = thin * (modelSubsurfaceThin(above, v, l0) * cap);
#if COV_COAT
        if (cover > 0 && NoV > 0)
            sun = keep * sun + cover * (shSunSpecular(1.0.xxx, coat.roughness, modelAlpha(coat.roughness), 1.0.xxx, n, v, NoV, l0, E, shPixelAngle(D, Dx)) *
                                            shCoatSunWeight(coat, v, l0, NoV, NoL, false) +
                                        (NoL > 0 ? modelCoatUnder(s, coat, n, v, l0) * above * cap : 0));
        if (sheenOn) sun = keepS * sun + sheen.color * (modelSheenSun(sheen.roughness, n, v, l0, g_sunAngularRadius) * cap);
#endif
        if (eye.mask > 0 && NoV > 0) sun += front * cap * (modelEyeCosine(eye, above, l0) - above);
        radiance += sun * sunVisibility;
    }

    // ---- local lights of the froxel list at the fragment's depth
    FroxelSrvs froxels;
    froxels.lights = P[3].x;
    froxels.lightIndices = P[3].x;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    // shading.mega_lights (render A; P[11].xy = the coverage layer's MegaLights instance, MegaLightsCoverage.hlsl: the
    // local lights' diffuse and specular at the pixel's nearest fragment from light samples with shadow rays, filtered,
    // exposed and divided by the modulation factors): every fragment of the pixel takes them times its own factors, in
    // place of the loop over the list with S's fragment visibility (S assigns no local shadow maps under mega_lights).
    if (P[11].x != UNX_NONE && (experiment & 32) == 0)
    {
        Texture2D<float4> covDiffuse = ResourceDescriptorHeap[P[11].x];
        Texture2D<float4> covSpecular = ResourceDescriptorHeap[P[11].y];
        if (NoV > 0 || foliage)
            radiance += (covDiffuse[pixel].rgb * mlDiffuseFactor(s.baseColor, s.metallic) +
                         covSpecular[pixel].rgb * mlSpecularFactor(s.baseColor, s.metallic, s.specular, s.roughness, abs(NoV))) / g_exposure;
    }
    else if (froxels.lights != UNX_NONE && (experiment & 32) == 0)
    {
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), lobeRoughness);
        const float3 compensation = 1 + f0 * (1 / e - 1);
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        const uint indexBase = froxelIndexBase(froxels);
        float zNear = linearZ, zFar = linearZ;
        if (shadow.valid)
        {
            Texture2D<uint2> depthRange = ResourceDescriptorHeap[P[7].x];
            const uint2 dr = depthRange[pixel];
            zNear = linearDepth(asfloat(dr.x));
            zFar = linearDepth(asfloat(dr.y));
        }
#if AREA
        const float3x3 frame = shShadingFrame(n, v, NoV);
        const float3x3 frameBack = float3x3(frame[0], -frame[1], -frame[2]);
        // (the Subsurface class: lobe 0's LTC and albedo here, lobe 1's beside them, the albedos weighed by the mix)
        const float3x3 specular = mul(shLtcInverse(P[3].y, max(NoV, 1e-4), subsurface ? skin.roughness0 : s.roughness), frame);
        float3 specularAlbedo = shSpecularAlbedo(f0, max(NoV, 1e-4), subsurface ? skin.roughness0 : s.roughness);
        float3x3 specular1 = frame;
        float3 specularAlbedo1 = 0;
        if (subsurface)
        {
            specularAlbedo *= skin.mix;
            specular1 = mul(shLtcInverse(P[3].y, max(NoV, 1e-4), skin.roughness1), frame);
            specularAlbedo1 = (1 - skin.mix) * shSpecularAlbedo(f0, max(NoV, 1e-4), skin.roughness1);
        }
        specularAlbedo *= keepCloth;
#if COV_COAT
        float3x3 coatSpecular = frame, coatBase = frame;
        float coatAlbedo = 0;
        float3 coatBaseAlbedo = 0;
        if (cover > 0 && NoV > 0)
        {
            const float rEq = modelCoatBaseRoughness(s, coat, NoV);
            coatSpecular = mul(shLtcInverse(P[3].y, max(NoV, 1e-4), coat.roughness), frame);
            coatBase = mul(shLtcInverse(P[3].y, max(NoV, 1e-4), rEq), frame);
            coatAlbedo = modelCoatEms(coat, NoV);
            coatBaseAlbedo = shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), rEq);
        }
#endif
#endif
        uint3 nearCasters = 0xFFFFFFFFu, farCasters = 0xFFFFFFFFu;
        if (shadow.valid)
        {
            nearCasters = shadowFirstCasters(froxels, pixel, zNear);
            farCasters = shadowFirstCasters(froxels, pixel, zFar);
        }
        uint4 lightWords = 0;
        // L2c (14.1c): the tile's FAR field at this record's depth interval: FAR lights of bins above the horizon (with
        // margin) are in the field, bins below add 0, straddling bins' lights and NEAR lights are evaluated here.
        bool fieldOn = false;
        uint fieldTile = 0, fieldK = 0;
        uint2 fieldNear = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
        if (P[5].w != UNX_NONE && g_covListed < P[10].y)
        {
            ByteAddressBuffer fieldBuf = ResourceDescriptorHeap[P[5].w];
            fieldTile = g_covListed * COV_TL_TILE_BYTES;
            const CovTlHeader h = covTlHeader(fieldBuf, fieldTile);
            if ((h.flags & 1u) != 0 && h.K > 0 && linearZ >= h.zMin)
            {
                fieldK = min((uint)(log(linearZ / h.zMin) / h.lnRatio), h.K - 1);
                // the record's own list must be the interval's (one froxel slice): else the per-record loop
                if (covTlSliceOf(fieldBuf, fieldTile, fieldK) == froxelSlice(froxelGrid(froxels.lights), linearZ))
                {
                    fieldOn = true;
                    fieldNear = covTlMask(fieldBuf, fieldTile, fieldK);
                }
            }
        }
        // (lighting channels, Scene.hlsli: the fragment's instance; the FAR field holds only lights that light every instance)
        g_lightChannels = instanceLightingChannels(loadInstance(id.instance).flags);
        for (uint i = 0; i < range.y; ++i)
        {
            const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
            if (fieldOn && i < 64 && (((i < 32 ? fieldNear.x : fieldNear.y) >> (i & 31)) & 1u) == 0)
            {
                // FAR: above or below its bin's horizon band -> in the field or 0; straddling -> evaluated here
                ByteAddressBuffer fieldBuf = ResourceDescriptorHeap[P[5].w];
                const float nb = dot(n, covTlBinDir(covTlBinId(fieldBuf, fieldTile, fieldK, i)));
                if (nb > COV_TL_SIN_HALF + COV_TL_MARGIN || nb < -(COV_TL_SIN_HALF + COV_TL_MARGIN)) continue;
            }
            const GpuLight light = loadLight(lightIndex);
            const float visibility = covLocalVisibility(shadow, nearCasters, farCasters, lightIndex);
            if (lightType(light) > LIGHT_SPOT)
            {
#if AREA
                const float3 p = (light.position - g_cameraPosition) - offset;
                const float window = shAreaWindow(light, p);
                if (window <= 0) continue;
                const float3 Lw = shAreaColor(light, p) * (light.intensity * window * visibility);
                uint first = NoV > 0 ? 0 : 2, last = foliage ? 3 : 2;
                const bool specularInReflections = shSpecularInReflections(P[7].y, lightIndex);  // P[7].y: B2 mask
                float scaleBase = 1;
#if COV_COAT
                // as ShadeOpaque LAYERED (MATERIAL_LAYERS 3.1): integrals 3 (coat lobe) and 4 (the base lobe through the
                // coat) in the same loop; the diffuse-like part through the coat reuses integral 0
                float3 coatAdd = 0;
                float coatId = 0, tvtl = 0;
                if (sheenOn) scaleBase = keepS;  // (the sheen lobe on area lights: its LTC, sheen step 3)
                if (cover > 0 && NoV > 0)
                {
                    scaleBase = keep;
                    last = 5;
                    const float muL = max(dot(n, normalize(p)), 1e-4);
                    tvtl = (1 - modelCoatEms(coat, NoV)) * (1 - modelCoatEms(coat, muL)) / (coat.eta * coat.eta);
                }
#endif
                if (subsurface && NoV > 0 && skin.mix < 1) last = 6;  // integral 5: the Subsurface class's lobe 1 (3, 4: the coat's)
                [loop] for (uint j = first; j < last; ++j)
                {
                    if ((j == 1 || j >= 3) && specularInReflections) continue;
                    if (j == 2 && !foliage) continue;
                    if (subsurface && (j == 3 || j == 4)) continue;
#if COV_COAT
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (j == 3 ? coatSpecular : (j == 4 ? coatBase : (j == 5 ? specular1 : (NoV > 0 ? frameBack : frame)))));
#else
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (j == 5 ? specular1 : (NoV > 0 ? frameBack : frame)));
#endif
                    const float I = shAreaIntegral(light, p, T, j == 0 || j == 2);
                    if (j == 0 && eye.mask > 0 && NoV > 0)
                    {
                        const float3x3 irisFrame = shShadingFrame(eye.iris, v, dot(eye.iris, v));
                        const float irisI = shAreaIntegral(light, p, irisFrame, true);
                        radiance += scaleBase * Lw * front * (SH_PI * eye.mask * (irisI * modelEyeCaustic(eye, normalize(p)) - I));
                    }
                    if (j == 5)
                    {
                        radiance += Lw * (specularAlbedo1 * I);
                        continue;
                    }
#if COV_COAT
                    if (j == 0) coatId = I;
                    if (j >= 3)
                    {
                        coatAdd += (j == 3 ? coatAlbedo : tvtl * coatBaseAlbedo) * I;
                        continue;
                    }
#endif
                    radiance += scaleBase * Lw * (j == 0 ? front * (SH_PI * I) : (j == 1 ? specularAlbedo * I : back * (SH_PI * I)));
                }
#if COV_COAT
                if (last == 5)
                {
                    const float muIn = modelCoatRefractedCos(max(dot(n, normalize(p)), 1e-4), coat.eta);
                    coatAdd += tvtl * (diffuse + modelCoatReturned(s, coat, muIn) / SH_PI) * (SH_PI * coatId);
                    radiance += cover * Lw * coatAdd;
                }
#endif
#endif
                continue;
            }
            float3 l;
            const float3 toLight = (light.position - g_cameraPosition) - offset;
            float3 El = shPunctualIlluminance(light, toLight, l);
            if (P[8].x != UNX_NONE)  // A8: the light's function towards this fragment (as ShadeOpaque)
                El *= lightFunction(P[8].x, lightIndex, light.forward, light.right, -l, linearZ * (2 * g_tanHalfFovY / g_viewHeight) / max(length(toLight), 1e-4), g_time);
            const float cosL = dot(n, l);
            float3 f = 0;
            if (subsurface)
            {
                // the Subsurface class's lobes; the far side adds the light through thin parts (the fragment's visibility)
                if (NoV > 0 && cosL > 0) f = front + shSpecularSubsurface(f0, skin, compensation, n, v, l, NoV, cosL);
                else if (s.transmission > 0 && NoV * cosL < 0) f = thin * (modelSubsurfaceThin(abs(cosL), v, l) / abs(cosL));
            }
            else if (NoV > 0 && cosL > 0) f = front + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
#if COV_COAT
            // (the coat's and the sheen's lobes are specular light: the light's specular scale, ShadingCommon.hlsli)
            if (cover > 0) f = keep * f + cover * (modelCoatLobe(coat, n, v, l) * shLightSpecular() + modelCoatUnder(s, coat, n, v, l));
            if (sheenOn && cosL > 0) f = keepS * (f - (f - front) * sheen.cloth) + sheen.color * (modelSheenLobe(sheen.roughness, n, v, l) * shLightSpecular());
#endif
            radiance += f * El * (abs(cosL) * visibility);
            if (eye.mask > 0 && NoV > 0)
                radiance += front * El * ((modelEyeCosine(eye, cosL, l) - max(cosL, 0.0)) * visibility);
        }
        g_lightChannels = 7u;
        if (fieldOn)
        {
            // the field: bins above the horizon band for n (front), and for -n (the Foliage back side)
            ByteAddressBuffer fieldBuf = ResourceDescriptorHeap[P[5].w];
            float3 eFront = 0, eBack = 0;
            [loop] for (uint b = 0; b < COV_TL_BINS; ++b)
            {
                const float nb = dot(n, covTlBinDir(b));
                if (nb > COV_TL_SIN_HALF + COV_TL_MARGIN) eFront += covTlE(fieldBuf, fieldTile, fieldK, b);
                else if (foliage && nb < -(COV_TL_SIN_HALF + COV_TL_MARGIN)) eBack += covTlE(fieldBuf, fieldTile, fieldK, b);
            }
            if (NoV > 0) radiance += front * max(0.0, dot(n, eFront));
            if (foliage) radiance += back * max(0.0, dot(-n, eBack));
        }
    }

#endif
#if COV_PART == 1 && COV_PART_EXPOSED
    return radiance * airTransmittance * g_exposure;  // (the in-scattering is part 2's)
#elif COV_PART == 1
    return radiance;  // before the air (CoverageSpecial MODE 5 keeps it for MODE 6)
#endif
    // ---- indirect: R's screen probes from the tile cache (irradiance; the K path for the specular lobe)
    if (P[4].w != UNX_NONE && (experiment & 6) != 6)
    {
        ProbeSrvs probes;
        probes.probes = P[4].w;
        probes.occlusion = P[4].w;
        probes.pad0 = P[5].x;
        probes.pad1 = P[6].z != UNX_NONE ? P[6].z + 1 : 0;  // R's GI cache
        const float3 nv = NoV > 0 ? n : -n;
        const float3 r = reflect(-v, n);
        const bool wantRadiance = NoV > 0 && (experiment & 4) == 0;
#if COV_PRESHADE_CLASSES || COV_PRESHADE_LIGHT
        // (one thread per record, no tile group: the probes straight from R's texture, the values the tile cache copies)
        const ScreenProbeLighting g = screenProbeGather(probes, pixel, worldPos, nv, linearZ, foliage && (experiment & 2) == 0, wantRadiance, r,
                                                        reflectionLobeHalfAngle(s.roughness, NoV));
#else
        const ScreenProbeLighting g = screenProbeGatherTile(probes, pixel / M_TILE, pixel, worldPos, nv, linearZ, foliage && (experiment & 2) == 0, wantRadiance, r,
                                                            reflectionLobeHalfAngle(s.roughness, NoV));
#endif
#if COV_COAT
        if (cover > 0 && NoV > 0)
        {
            // as ShadeOpaque LAYERED: the coat cone's K-path radiance, the base through the coat
            const float3 irr = (experiment & 2) == 0 ? g.irradiance * g.occlusion : 0;
            const float3 inc = wantRadiance ? g.radiance : 0;
            const float3 coatIncident = (experiment & 4) == 0 ? screenProbeGather(probes, pixel, worldPos, nv, linearZ, false, true, r, reflectionLobeHalfAngle(coat.roughness, NoV)).radiance : 0;
            const float tv = 1 - modelCoatEms(coat, NoV), tBar = 1 - modelCoatLookup1(coat.coat * MODEL_COAT_STRIDE + 4096, coat.roughness);
            const float3 under = tv * tBar * ((front + modelCoatReturned(s, coat, modelCoatRefractedCos(2.0 / 3.0, coat.eta)) / SH_PI) * irr +
                                              inc * shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), modelCoatBaseRoughness(s, coat, NoV)));
            radiance += keep * (front * irr + inc * shSpecularAlbedo(f0, NoV, s.roughness)) + cover * (under + coatIncident * modelCoatEms(coat, NoV));
        }
        else if (sheenOn)
        {
            // as ShadeOpaque LAYERED=2: the base's indirect light scaled, the sheen's C E_sh(n.v) E / pi
            const float3 irr = (experiment & 2) == 0 ? g.irradiance * g.occlusion : 0;
            radiance += keepS * (front * irr + (wantRadiance ? g.radiance * (shSpecularAlbedo(f0, NoV, s.roughness) * keepCloth) : 0)) +
                        sheen.color * (modelSheenAlbedo(NoV, sheen.roughness) / SH_PI) * irr;
        }
        else
#endif
        {
            if ((experiment & 2) == 0)
            {
                if (NoV > 0) radiance += front * (g.irradiance * g.occlusion);
                radiance += back * (g.irradianceBack * g.occlusion);
            }
            if (wantRadiance) radiance += g.radiance * shSpecularAlbedo(f0, NoV, s.roughness);
        }
    }
    else if (giSourceIsVolume(P[6].z) && (experiment & 6) != 6)
    {
        // gi.lumen_only: a fragment is not the pixel's opaque surface, so the final gather has nothing for it - the
        // translucency volume at the fragment: irradiance on each side, the lobe's radiance from the mirror direction
        const LtvSh sh = ltvSample(ltvParams(giSourceVolume(P[6].z)), worldPos);
        const float3 nv = NoV > 0 ? n : -n;
        float3 irr = (experiment & 2) == 0 ? ltvIrradianceOf(sh, nv) : 0;
        if ((experiment & 2) == 0 && NoV > 0 && eye.mask > 0)
            irr = lerp(irr, ltvIrradianceOf(sh, eye.iris), eye.mask);
        const float3 inc = NoV > 0 && (experiment & 4) == 0 ? ltvRadianceOf(sh, reflect(-v, n)) : 0;
#if COV_COAT
        if (cover > 0 && NoV > 0)
        {
            const float tv = 1 - modelCoatEms(coat, NoV), tBar = 1 - modelCoatLookup1(coat.coat * MODEL_COAT_STRIDE + 4096, coat.roughness);
            const float3 under = tv * tBar * ((front + modelCoatReturned(s, coat, modelCoatRefractedCos(2.0 / 3.0, coat.eta)) / SH_PI) * irr +
                                              inc * shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), modelCoatBaseRoughness(s, coat, NoV)));
            radiance += keep * (front * irr + inc * shSpecularAlbedo(f0, NoV, s.roughness)) + cover * (under + inc * modelCoatEms(coat, NoV));
        }
        else if (sheenOn)
            radiance += keepS * (front * irr + inc * (shSpecularAlbedo(f0, NoV, s.roughness) * keepCloth)) + sheen.color * (modelSheenAlbedo(NoV, sheen.roughness) / SH_PI) * irr;
        else
#endif
        {
            if (NoV > 0) radiance += front * irr;
            if ((experiment & 2) == 0) radiance += back * ltvIrradianceOf(sh, -nv);
            radiance += inc * shSpecularAlbedo(f0, NoV, s.roughness);
        }
    }
    return (radiance * airTransmittance + airInscatter) * g_exposure;
}

// A fragment's exposed radiance in the composite: a cluster record is shaded here, a special record (top bits != 00) is
// read from ViewResources::coverageRecordRadiance (P[8].y), where its owner shaded it before the composite (v1.75).
float3 covFragmentRadiance(uint visId, uint element, uint2 pixel, uint experiment)
{
    if ((visId >> 30) != 0)
    {
#if COV_PART == 1 && COV_PART_EXPOSED
        return 0;  // (a pre-shaded record's value is whole: counted once, in part 2)
#endif
        if (P[8].y == UNX_NONE) return 0;  // (a view without V's special list: nobody shaded it; never a cluster decode)
        ByteAddressBuffer shaded = ResourceDescriptorHeap[P[8].y];
        return covUnpackRadiance(shaded.Load2(element * 8));
    }
    return covShadeFragment(visId, element, pixel, experiment);
}

#endif
