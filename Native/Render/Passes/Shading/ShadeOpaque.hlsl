// unx-kernel: cs_6_6 main
// unx-variants: FALLBACK=0,1 AREA=0,1 PLANAR=0,1 LAYERED=0,1,2,3
// Two kernels since 2026-10-01 (the DXIL limit: the one kernel's FALLBACK variants stood at 204,260 of 204,800 B):
//   part 1 (this file as compiled, SHADE_PART 1): the per-pixel setup, emission, the sun, the local lights (slots, the
//          overflow list or S's VSM in fallback tiles), L2's tile FAR term and the 14.1b emissive irradiance -> the
//          direct radiance texture P[10].x (RGBA32F UAV: linear radiance before exposure, written for active pixels);
//   part 2 (ShadeIndirect.hlsl, SHADE_PART 2): the same setup, then R's indirect light, the A9 lobe texture, the air
//          and the output (OUTPUT, the exposure histogram, particles, edge / coverage radiance).
// The sum continues where part 1 stopped through a float32 store and load, so the two kernels equal the one kernel bit
// for bit; the setup is recomputed (deterministic) rather than stored.
// (14.1 NEAR/FAR: Passes/Common/LightNearFar.hlsli is the shared classification; the tile FAR term comes with L2.)
// Shading kernel of the opaque classes (ARCHITECTURE 2.11; INTERFACES 5.6, 7, 8): one 8 x 8 tile of the class's tile
// list per group (ExecuteIndirect), pixels of other classes skipped. Per pixel, from the G-buffer, depth and material
// word (no vis-buffer re-derivation):
//   sun      model BRDF over the solar disk (ShadingCommon.hlsli shSunSpecular), S's shadow slot 0, S's transmittance;
//   GI       R's screen probes: diffuse irradiance x near occlusion; Foliage transmission from the back hemisphere;
//   specular R's reflection (G/M paths, planar mirrors) or the K path screenProbeRadiance with the lobe half-angle,
//            times the model's specular directional albedo;
//   emission material (x emissive texture, resolved per pixel);
//   air      S's air volume between the camera and the surface (main view, atmosphereAirView: atmosphere, casters'
//            shadows in the air, local lights' air) and the sun's illuminance at the surface;
// then exposure, tone map and the final 4 B (OUTPUT=0) or linear radiance x exposure (OUTPUT=1).
// Classes without their own model yet (Water: INTERFACES 8.1 defines it before P4) use this kernel. Subsurface has its
// own part 1 (LAYERED=3 below) and this kernel's part 2 (its indirect specular is the Standard lobe's).
// Local lights: the pixel's froxel list (S, Froxel.hlsli), punctual lights exactly (INTERFACES 8.2); shadow-casting
// lights take S's slots 1-3 in list order (7.3); those past the third read S's overflow list (7.3, v1.20: tile head,
// pixel record, 8-bit run), loads only. Tiles over the list's capacity are left to FALLBACK=1, which runs on S's fallback
// tile list, shades every non-sky class there and evaluates the lights past the third with S's VSM directly (outside
// the main kernel, so its registers stay as they are). Area lights: AreaLight.hlsli, compiled in (AREA=1) only for scenes
// that have area lights (their registers lowered occupancy for every pixel otherwise: +0.23 ms at city 4K, measured). Every view uses its own froxel lists
// and air volume (v1.22; planar reflection views get them from S), taken by whether the SRVs are present. Indirect light
// comes from R's screen probes in the main view (PLANAR=0) and from R's world cache in planar reflection views (PLANAR=1);
// each kernel compiles only its own path.
// P[0] = { gbuffer, depth, material word, color UAV }
// LAYERED: 1 = A9 clearcoat (shade class Layered), 2 = A9 sheen (shade class Sheen; MATERIAL_LAYERS 1.4), 3 = the
//        Subsurface class's model (shade class Subsurface; MaterialModel.hlsli ModelSubsurface) - no A9 layer: the
//        variant compiles as LAYERED 0 with SUBSURFACE 1, part 1 only (and MegaLightsShade). Its pixels are all of that
//        class (the tile list's, or the fallback run's class mask), so the record's class slots are read without a test:
//          sun      each of the two lobes by the disk rules at its own roughness (as the Standard lobe), weighed by the
//                   mix, the compensation at the average roughness;
//          points   the model exactly (the two lobes' D, V and the compensation at the average roughness);
//          area     each lobe's LTC integral and albedo, weighed by the mix (two Standard lobes);
//          thin     transmission > 0: the light through thin parts (modelSubsurfaceThin) from the sun and from point and
//                   spot lights on the far side of the shading normal - the light's one visibility decides, so a part
//                   thicker than the shadow bias shadows itself -, and from area lights: the far side's cosine integral
//                   times W over the cosine at the light's centre; not from the emissive irradiance or the indirect light.
//          eye      a Subsurface material with an iris (MATERIAL_EYE; MaterialModel.hlsli "Eye"): one lobe - the cornea's,
//                   at the surface normal -, and on the iris mask's share every direct diffuse term on the iris plane
//                   with the caustic (modelEyeCosine; the plane's normal, the mask and the caustic weight from the class
//                   word P[9].y - without that texture an eye is plain) in place of the surface's cosine: the sun at the disk's centre,
//                   point and spot lights, area lights by a cosine integral in the plane's frame. The indirect diffuse
//                   light on the mask's share is the iris plane's (part 2 with SSS_SPLIT): with gi.lumen_only the final
//                   gather's irradiance x the translucency volume's ratio E(a) / E(n) (the gather keeps its detail and
//                   occlusion, the volume turns it to the plane; the ratio held to [1/4, 4]), in planar views the
//                   volume's or the world cache's irradiance at a; the screen-probe path keeps the surface's. The tile
//                   term and the emissive irradiance stay the surface's. The scatter pass leaves the iris its own light
//                   (the mean free path x (1 - mask)).
//        2 (the sheen class) carries the cloth blend (MaterialModel.hlsli modelEvaluateSheen): every specular term of the
//                   base - the sun's lobe, point and spot lights, the area lights' LTC lobe, the indirect lobe - x (1 - cloth).
// SSS_SPLIT = 1 (shading.subsurface_scatter, SubsurfaceScatter.hlsli states the passes): the Subsurface class's kernels
//        with the diffuse light apart. Parts 1 and 2 (SubsurfaceDirect.hlsl, SubsurfaceIndirect.hlsl) sum the specular
//        light and the emission as before and add every diffuse term per unit f_d - the sun's cosine, the local lights',
//        the tile term, the emissive irradiance, the light through thin parts, the indirect irradiance - x exposure to
//        the class's diffuse texture P[9].z (RGBA16F UAV); part 2 writes the first sum back to P[10].x and the pixel's
//        view depth to P[9].z's alpha, and no output. P[10].y is then the local lights' specular alone
//        (MegaLightsSpatialSubsurface.hlsl). Part 3 (SubsurfaceScatter.hlsl, SHADE_PART 3) is the same setup, then
//        colour = P[10].x (an SRV there) + f_d x the scattered diffuse light (P[9].z as an SRV; P[11].y samples per
//        pixel, P[11].z the footprint in pixels under which a pixel keeps its own: SubsurfaceScatter.hlsli sssScatter)
//        and this file's air and output; it reads P[0], P[1].xyz, P[3], P[4].zw, P[5].zw, P[6].xyz, P[7].xw.
// P[1] = { tile lists (raw), list offset (entries), shade class (bit 31 set: a mask of classes, 0xFFFFFFFF every non-sky
//        class - the fallback kernel runs once per LAYERED variant over its classes), emissive or
//        UNX_NONE }
// P[2] = { shadow visibility, screen probes, reflection, GI cache (planar views; with the screen probes their fallback)
//        or, without screen probes, the indirect light's source word (GiSource.hlsli: the Lumen translucency volume) }
//        (UNX_NONE = absent)
// gi.lumen_only (no screen probes; part 2): P[9].w = the final gather's diffuse irradiance, P[11].y = its rough specular
//        (view.giRoughSpecular: RGBA16F radiance x exposure; UNX_NONE: none), P[11].z = the short-range AO
//        (view.shortRangeAO, LumenShortRangeAO.hlsli; UNX_NONE: none), P[11].w = the gather's Foliage back-side
//        irradiance (view.giBackfaceIrradiance: x exposure; UNX_NONE: none - the translucency volume then)
// P[3] = { atmosphere transmittance, multi-scatter, S's shadow overflow tile heads (main kernel; UNX_NONE = absent), this
//        view's air volume } (this kernel reads no sky view)
// P[4] = { B2 stable area lights' mask (raw, 1 bit per scene light; UNX_NONE = none), texture table, experiment mask (0;
//        shading.toml), exposure histogram UAV (part 2; UNX_NONE = not metered) }
// P[11].x L2 tile lights' records (raw, TileLights.hlsli; UNX_NONE = off). (Until 2026-10-01 part 1 read P[4].w, the
//        exposure histogram's UAV in the record: never the tile records.)
// P[10].w L3 S's tile lit records (raw, VsmCls.hlsli; UNX_NONE = off)
// P[10].z 14.1b emissive area lights' diffuse irradiance (RGBA16F, exposed; Passes/Lights/EmissiveDirect.hlsl; UNX_NONE =
//        off). (Until 2026-10-01 the kernel read it from P[4].x, B2's word: the term was never added.)
// P[5] = { froxel lights (raw) (UNX_NONE = absent), LTC table (StructuredBuffer<float4>, AreaLight.hlsli) }
// P[6] = { edge tile mask SRV (EdgeDetect.hlsl, R32G32_UINT per tile; UNX_NONE = no edge pixels), V's coverage tiles
//        (raw; UNX_NONE = no coverage layer: a tile with coverage fragments keeps every pixel's exposed radiance for the
//        coverage composite, CoverageComposite.hlsl), exposure histogram's centre sigma, E's light function table (raw;
//        UNX_NONE = none: A8 cookies, IES, gobos, animated intensity and colour on point and spot lights) }
// P[7] = { edge radiance UAV (RGBA16F), R's screen probe maps (K path; UNX_NONE = absent), S's shadow overflow list (raw;
//        FALLBACK: a raw buffer holding this frame's ShadowSrvs), V's water layer vis ids (v1.75; UNX_NONE = none): a
//        pixel under a water-layer stream surface keeps its radiance too, W's refraction source (tracks::water) }
// P[9].w R's per-pixel front GI irradiance (view.giIrradiance, RGBA16F: rgb x exposure, a = cache data; UNX_NONE: none):
//        replaces the cache lookup in the probe gather (main view) or the direct lookup (planar views: R's
//        r.gi.screen.planar); Foliage keeps the cache for its back side
// P[9].z A9 area-light lobe texture (RGBA16F UAV, exposed radiance; AreaLobes.hlsl writes it, the LAYERED variants with
//        AREA read it; UNX_NONE = none)
// P[9].y A9 anisotropy word (Resolve.hlsl; UNX_NONE = no anisotropic and no eye material): read by the LAYERED variants,
//        whose anisotropic pixels shade the base specular with the anisotropic lobe (AnisoShading.hlsli), and by the
//        Subsurface variant for an eye's pixels (the eye word; SHADE_PART 3 reads its mask)
//        and by every variant's part 1 for the pixels of a height-mapped material: the sun's visibility through the
//        height field (material.parallax_shadow; bits 0..7)
// P[11].zw (part 1 without MEGA_LIGHTS: the froxel lists' light loop) the vis buffer and V's visible clusters (UNX_NONE:
//        none): the pixel's instance gives the receiver's lighting channels (Scene.hlsli g_lightChannels), so a light
//        in none of them adds nothing. Part 2 reads the gather's textures there (above); with MEGA_LIGHTS the sampling
//        tests the channels (MegaLightsSample.hlsl).
// P[8].xy (SHADE_PART 3) the vis buffer and V's visible clusters in frames whose view models are drawn through
//        viewmodel.fov_override_degrees (UNX_NONE: none): a view-model pixel scatters at its true size
// P[10].y the stochastic local lights' result (shading.mega_lights, MegaLights.hlsli; RGBA16F exposed radiance, m.ml.spatial;
//        UNX_NONE = off): part 1 then skips its local-light loop and part 2 adds the texture. (MEGA_LIGHTS = 1: P[10].y..P[11]
//        as MegaLightsShade.hlsl states.)
// P[8] = { W's sun-space water map (v1.77): waterSunDepth, waterSunNormal, waterSunMedium, waterSunConstants (UNX_NONE:
//        no water) } - a surface under water from the sun takes the refracted sun direction and the water's transmittance
//        (Passes/Water/WaterLight.hlsli waterSunLight); P[9].x its caustics (waterSunCaustics, UNX_NONE: none)
#ifndef AREA_LOBES
#define AREA_LOBES 0  // AreaLobes.hlsl compiles this file with 1: the area-light lobe terms alone (see there)
#endif
#ifndef SHADE_PART
#define SHADE_PART 1  // ShadeIndirect.hlsl compiles this file with 2 (see the header)
#endif
#ifndef ML_FULLSCREEN
#define ML_FULLSCREEN 0
#endif
#ifndef MEGA_LIGHTS
#define MEGA_LIGHTS 0  // MegaLightsShade.hlsl compiles this file with 1: the local lights of the pixel's light samples alone
#endif
#ifndef OUTPUT
#define OUTPUT 0      // (part 1 writes no output)
#endif
#ifndef SSS_SPLIT
#define SSS_SPLIT 0   // SubsurfaceDirect.hlsl, SubsurfaceIndirect.hlsl and SubsurfaceScatter.hlsl compile this file with 1 (see the header)
#endif
#if LAYERED == 3
#define SUBSURFACE 1  // the Subsurface class's variant (see the header)
#undef LAYERED
#define LAYERED 0
#else
#define SUBSURFACE 0
#endif
#if LAYERED == 1
#define MODEL_FILM 1  // A9 thin film (MaterialModel.hlsli modelFresnel)
#endif
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Lights/LightFunction.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#if LAYERED
#include "Passes/Shading/AnisoShading.hlsli"
#if AREA_LOBES || (MEGA_LIGHTS && AREA)
#include "Passes/Shading/AreaQuadrature.hlsli"
#endif
#endif
#include "Passes/Water/WaterLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Common/LightNearFar.hlsli"  // 14.1 NEAR/FAR classification and FAR vector irradiance (shared header)
#include "Passes/Lights/TileLights.hlsli"       // L2: the tile's NEAR mask and corner irradiance (shading.tile_lights)
#include "Passes/Shadow/VsmCls.hlsli"            // L3: S's (tile, light) lit classification (P[10].w; a lit caster needs no visibility read)
#include "Passes/Visibility/CoverageTiles.hlsli"
#if !PLANAR
#define GI_PROBE_TILE_CACHE  // R's screen probes at the group's tile corners, loaded once (design revision 1 4.4, 12.3)
#endif
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#if (SHADE_PART == 2 || SHADE_PART == 4) && !AREA_LOBES
#include "Passes/GI/GiSource.hlsli"
#include "Passes/GI/LumenShortRangeAO.hlsli"
#endif
#if MEGA_LIGHTS
#include "Passes/Shading/MegaLightsUpsample.hlsli"
#endif
#if SHADE_PART == 3
#include "Passes/Shading/SubsurfaceScatter.hlsli"
#endif

struct ShadedPixel
{
    float3 radiance;  // linear, before exposure
#if SSS_SPLIT && SHADE_PART == 1
    float3 scatter;   // SSS_SPLIT part 1: the diffuse light per unit f_d (linear, before exposure)
#endif
};

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, uint2 gbPacked, float depthValue,
                         uint overflowHead);


[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
#if ML_FULLSCREEN
    const uint2 tileCoord = gid.xy;  // (MegaLightsShade.hlsl FULL = 1: every tile of the view)
#else
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
    const uint tile = tiles.Load(4 * (P[1].y + gid.x));
    const uint2 tileCoord = uint2(tile & 0xFFFFu, tile >> 16);
#endif
    const uint2 pixel = tileCoord * M_TILE + tid;
    // The group's independent reads go out together before any of them is waited on: the records of the tile's 2 x 2 corner
    // screen probes (R's tile cache, split form: the probe counts come from the frame constants, so no header read comes
    // first), S's overflow tile head, and the pixel's material word, G-buffer and depth. The records then go to groupshared
    // [measured, city 4K: shading 1.257 -> 1.191 ms against loading the tile, then the pixel]. The probe condition is
    // uniform (root constants), so the whole group reaches the barrier.
    const uint lane = tid.y * M_TILE + tid.x;
#if (SHADE_PART == 2 || SHADE_PART == 4) && !PLANAR
    const bool probeTile = !AREA_LOBES && P[2].y != UNX_NONE && (P[4].z & 6) != 6;
    ProbeSrvs probes;
    probes.probes = P[2].y;
    probes.occlusion = P[2].y;
    probes.pad0 = P[7].y;
    probes.pad1 = P[2].w != UNX_NONE ? P[2].w + 1 : 0;  // R's GI cache (irradiance from the cache map), UNX_NONE: none
    uint4 probeRecord = 0;
    if (probeTile) probeRecord = giProbeTileFetch(probes, tileCoord, lane, giProbeCountOfView());
#endif
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    const bool inView = all(pixel < uint2(g_viewWidth, g_viewHeight));
    const uint2 readPixel = min(pixel, uint2(g_viewWidth, g_viewHeight) - 1);
    const uint wordRead = words[readPixel];
    const uint2 gbPacked = gbuffer[readPixel];
    const float depthValue = depthTex[readPixel];
    uint coverageFragments = 0;  // V's coverage fragments of this tile (the band A radiance is kept for the composite)
    if (P[6].y != UNX_NONE)
    {
        ByteAddressBuffer coverage = ResourceDescriptorHeap[P[6].y];
        coverageFragments = coverage.Load(4 * ((tileCoord.x + tileCoord.y * ((g_viewWidth + 7) / 8)) * COV_TILE_WORDS + COV_TILE_COUNT));
    }
    uint overflowHead = 0;  // S's overflow tile head (7.3): 0 none, 1 + block start
#if !FALLBACK && !MEGA_LIGHTS
    if (P[3].z != UNX_NONE)
    {
        Texture2D<uint> heads = ResourceDescriptorHeap[P[3].z];
        overflowHead = heads[tileCoord];
        if (overflowHead == 0xFFFFFFFFu) return;  // over the list's capacity: the fallback kernel shades this tile
    }
#endif
#if (SHADE_PART == 2 || SHADE_PART == 4) && !PLANAR
    if (probeTile)
    {
        giProbeTileStore(lane, probeRecord);
        GroupMemoryBarrierWithGroupSync();
    }
#endif
    const uint word = inView ? wordRead : M_MATERIAL_SKY;
    const uint materialIndex = mWordMaterial(word);
    bool active = inView && materialIndex != M_MATERIAL_SKY;
    const GpuMaterial m = loadMaterial(active ? materialIndex : 0);
    const uint shadeClass = mShadeClassOf(m);
    active = active && (P[1].z >= 0x80000000u ? ((P[1].z >> shadeClass) & 1u) != 0 : shadeClass == P[1].z);
    ShadedPixel sp = (ShadedPixel)0;
    if (active) sp = shadeSurface(pixel, word, materialIndex, m, words, gbPacked, depthValue, overflowHead);
#if AREA_LOBES || MEGA_LIGHTS
    return;  // (the lobe texture / the light samples' diffuse and specular are written; edges keep the full kernel's radiance)
#endif
#if SHADE_PART == 1
    // part 1's result: the direct radiance of this class's pixels (other classes' pixels are left to their own runs)
    if (active)
    {
        RWTexture2D<float4> direct = ResourceDescriptorHeap[P[10].x];
        direct[pixel] = float4(sp.radiance, 1);
#if SSS_SPLIT
        // the class's diffuse light per unit f_d, exposed (f16 range), onto what m.ml.spatial.sss left there (a stays 0
        // until part 2 has added its share)
        RWTexture2D<float4> scatterDiffuse = ResourceDescriptorHeap[P[9].z];
        scatterDiffuse[pixel] = float4(min(scatterDiffuse[pixel].rgb + sp.scatter * g_exposure, 60000.0), 0);
#endif
    }
    return;
#endif
#if SSS_SPLIT && SHADE_PART == 2
    return;  // (the scatter kernel writes the output and keeps the edge / coverage radiance)
#endif

    // ---- edge (E) pixels (EdgeDetect.hlsl marked them in the tile's mask) and the pixels of coverage tiles keep their
    // exposed linear radiance for the composites (EdgeComposite.hlsl, CoverageComposite.hlsl).
    if (!active) return;
    bool keep = coverageFragments != 0;
    if (!keep && P[6].x != UNX_NONE)
    {
        Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[6].x];
        const uint2 edgeMask = edgeTiles[tileCoord];
        const uint bit = tid.y * M_TILE + tid.x;
        keep = (((bit < 32 ? edgeMask.x : edgeMask.y) >> (bit & 31)) & 1u) != 0;
    }
    if (!keep && P[7].w != UNX_NONE)
    {
        Texture2D<uint> waterVis = ResourceDescriptorHeap[P[7].w];
        keep = (waterVis[pixel] >> 30) == 3u;  // COV_STREAM_ID: W's water surfaces
    }
    if (keep)
    {
        RWTexture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[7].x];
        edgeRadiance[pixel] = float4(sp.radiance * g_exposure, 1);
    }
}

// Shades one pixel of this class (writes the output) and returns its linear radiance (kept for edge pixels).
// Visibility of the shadow-casting light at 'ordinal' (> 3) of the pixel's list order: S's overflow run (main kernel) or
// S's VSM estimator (fallback tiles). 1 without S's resources. 'record' caches the pixel's overflow record (main kernel)
// or marks the receiver as built (fallback: 'receiver', built once per pixel).
float shOverflowVisibility(uint2 pixel, uint ordinal, uint overflowHead, inout uint record, inout uint2 packedCache,
                           inout ShadowPixelReceiver receiver, uint lightIndex)
{
#if FALLBACK
    // S's own evaluation for the slots and the overflow list (the visibility pass's receiver: position, normal and
    // footprint of the pixel), quantised to 8 bits as S stores it: fallback tiles equal the list bit for bit.
    if (P[7].z == UNX_NONE) return 1;
    if (record == 0xFFFFFFFFu)
    {
        receiver = shadowPixelReceiver(pixel, P[0].y, P[0].x);
        record = 0;
    }
    if (receiver.valid == 0) return 1;
    ByteAddressBuffer b = ResourceDescriptorHeap[P[7].z];
    const uint4 a = b.Load4(0), c = b.Load4(16);
    ShadowSrvs vsm;
    vsm.pageTable = a.x;
    vsm.pool = a.y;
    vsm.blocks = a.z;
    vsm.searchBound = a.w;
    vsm.constants = c.x;
    vsm.lights = c.y;
    vsm.pad0 = c.z;
    vsm.layers = c.w;
    return round(saturate(shadowLocalVisibilityAtReceiver(vsm, lightIndex, receiver)) * 255.0) / 255.0;
#else
    if (overflowHead == 0 || P[7].z == UNX_NONE) return 1;
    ByteAddressBuffer b = ResourceDescriptorHeap[P[7].z];
    const uint block = overflowHead - 1;
    const uint j = ordinal - 4;
    if (j >= (record >> 24)) return 1;
    const uint word = j / 4;
    if (packedCache.x != word) packedCache = uint2(word, b.Load(4 * (block + (record & 0xFFFFFFu) + word)));
    const uint w = packedCache.y;
    return ((w >> (8 * (j & 3))) & 0xFFu) / 255.0;
#endif
}

// B2 (COVERAGE 12.4 structure 2): a stable area light's specular is in R's reflection paths in the main view (G/M rays
// see the emitters; the K path's maps carry the cache's emitter texels). Planar views read the cache without them
// (their giCacheRadiance is the texels alone, prefiltered: the emitter radiance at texel resolution drew squares there)
// and shade every light's specular by LTC.
bool shLightSpecularInResult(uint lightIndex)
{
#if PLANAR
    return false;
#else
    return shSpecularInReflections(P[4].x, lightIndex);
#endif
}

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, uint2 gbPacked, float depthValue,
                         uint overflowHead)
{
    const GBufferSample g = decodeGBuffer(gbPacked);
    const float linearZ = linearDepth(depthValue);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;  // camera-relative position (D has unit depth along the view axis)
    const float3 worldPos = g_cameraPosition + offset;
    const float3 v = -normalize(D);
    // The resolve bent the normal to n.v >= 1e-4 (mNormalTowardsViewer); the G-buffer's octahedral 2 x 16 bits can take
    // it back below: the same bend on the decoded normal (a no-op elsewhere).
    const float3 n = mNormalTowardsViewer(g.normal, v);
    const float NoV = dot(n, v);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = g.baseColor;
    s.roughness = g.roughness;
    s.metallic = mWordMetallic(word);
    s.specular = m.specular;
    s.transmission = m.transmission;
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);  // Lambert f_d before the Foliage split
#if LAYERED == 1
    // A9 thin film (MATERIAL_LAYERS 1.2; film materials are layered without a coat): every base modelFresnel below is F',
    // and f0 is F'(1) in the compensation and the albedo tables (MaterialModel.hlsli modelFilmBegin).
    const float3 f0 = modelFilmBegin(m, modelF0(s));
#else
    const float3 f0 = modelF0(s);
#endif
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
#if SSS_SPLIT
    // The diffuse light apart: every diffuse term below adds nothing to 'radiance' (its f_d is 0 here) and its light per
    // unit f_d to scatterE - the scatter pass's E_d.
    const float3 front = 0;
    float3 scatterE = 0;
#else
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
#endif
    const float3 back = foliage ? diffuse * s.transmission : 0;
#if SUBSURFACE
    // The Subsurface class's two specular lobes at the pixel's roughness, and f_d x transmission for the light through
    // thin parts (0: none).
    const ModelSubsurface skin = modelSubsurfaceOf(m, s.roughness);
#if SSS_SPLIT
    const float3 thin = 0;
#else
    const float3 thin = diffuse * s.transmission;
#endif
    // An eye's pixel (see the header): its mask, and outside the scatter kernel the iris plane's normal and the caustic
    // normal, from the eye word. mask 0: not an eye, or its sclera - the plain Subsurface model.
    ModelEye eye = (ModelEye)0;
    if ((m.classFlags & MATERIAL_EYE) != 0 && P[9].y != UNX_NONE)
    {
        Texture2D<uint> eyeWords = ResourceDescriptorHeap[P[9].y];
#if SHADE_PART == 3
        eye.mask = modelEyeMask(eyeWords[pixel]);
#else
        eye = modelEyeOf(eyeWords[pixel], n);
#endif
    }
#endif
#if LAYERED == 1
    // A9 clearcoat (MATERIAL_LAYERS 1.1, MaterialModel.hlsli): f = (1 - c) f_base + c (f_c + f_under); the coat's roughness
    // from the material word (footprint-filtered). The base terms below are scaled by (1 - c) at the end of each light.
    ModelCoat coat = modelCoatOf(m);
    coat.roughness = mWordCoatRoughness(word);
    const float cover = coat.cover, keep = 1 - cover;
#endif
#if LAYERED == 2
    // A9 sheen (MATERIAL_LAYERS 1.4, MaterialModel.hlsli): f = C f_sh + keepS f_base with keepS = 1 - max(C) E_sh(n.v) on the
    // viewer's side only, so every light's base term and the base's indirect light scale alike (exact); the sheen's
    // roughness from the material word (footprint-filtered, one layer kind per material).
    ModelSheen sheen = modelSheenOf(m);
    sheen.roughness = max(mWordCoatRoughness(word), 0.1);
    const float keepS = NoV > 0 ? modelSheenKeep(sheen, NoV) : 1;
    const float keepCloth = 1 - sheen.cloth;  // the cloth blend: what stays of the base's specular lobe
#endif
#if LAYERED
    // A9 anisotropy (MATERIAL_LAYERS 1.5; anisotropic materials are layered): the base's specular lobe (AnisoShading.hlsli)
    const ShAniso aniso = shAnisoOf(P[9].y, pixel, m, g.normal, n, v);
#endif

    AtmosphereSrvs atm;
    atm.transmittance = P[3].x;
    atm.multiScatter = P[3].y;
    atm.skyView = UNX_NONE;  // P[3].z carries the shadow overflow heads: this kernel reads no sky view
    atm.aerial = P[3].w;
    const bool haveAtmosphere = atm.transmittance != UNX_NONE;
    const uint experiment = P[4].z;

#if SHADE_PART == 3
    float3 radiance;
    {
        Texture2D<float4> direct = ResourceDescriptorHeap[P[10].x];  // SSS_SPLIT part 2's sum: the specular light and the emission
        radiance = direct[pixel].rgb;
    }
#elif SHADE_PART == 2
    float3 radiance;
    {
        RWTexture2D<float4> direct = ResourceDescriptorHeap[P[10].x];  // part 1's direct radiance (ShadeOpaque.hlsl)
        radiance = direct[pixel].rgb;
    }
    if (P[10].y != UNX_NONE)
    {
        Texture2D<float4> megaLights = ResourceDescriptorHeap[P[10].y];  // shading.mega_lights: the local lights (m.ml.spatial)
        radiance += megaLights[pixel].rgb / g_exposure;
    }
#elif AREA_LOBES || MEGA_LIGHTS
    float3 radiance = 0;
    if (false)
#else
    float3 radiance = m.emissive;
    // (the resolve's emissive texture, when the frame has one, holds every surface pixel's emission: the material's
    // x its texture and mask, + E's emissive decals - Resolve.hlsl, Decal.hlsli)
    if (P[1].w != UNX_NONE)
#endif
#if (SHADE_PART == 1 || SHADE_PART == 4)
    {
        Texture2D<float4> emissive = ResourceDescriptorHeap[P[1].w];
        radiance = emissive[pixel].rgb;  // material emissive x texture, resolved at the footprint, + emissive decals
    }
#endif

    // ---- sun (INTERFACES 8.1: reflection on the viewer's side of the shading normal, Foliage transmission across it)
    float3 l0 = normalize(g_sunDirection);
    // S's air volume of this view (v1.22; planar views: integrated from the mirror plane on) gives the air between the
    // camera and the surface and the sun's illuminance at it in one lookup (atmosphereAirView); without one, the sun's
    // transmittance alone.
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (haveAtmosphere)
    {
        if (atm.aerial != UNX_NONE && (P[4].z & 8) == 0)
            atmosphereAirView(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
#if (SHADE_PART == 1 || SHADE_PART == 4) && !MEGA_LIGHTS
    float sunVisibility = 1;
    if (P[2].x != UNX_NONE && (P[4].z & 2048) == 0)
    {
        Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
        sunVisibility = shadowSlot(shadow[pixel], 0);
        // S's glass casters (shadow.vsm.translucent_tint): slot 0 has the luminance of what they let through, the
        // texture's second half its colour
        E *= shadowSunTintChroma(shadow, pixel);
    }
    // material.parallax_shadow (MaterialInputs.hlsli mParallax): the pixel's height field hides the sun - the resolve left
    // the visibility in the class word of a height-mapped material's pixels (not an anisotropic material's or an eye's:
    // the word is theirs)
    // (nor a Cut or Terrain material's: the resolve marches no height field there and writes no word)
    if (m.inputs != UNX_NONE && P[9].y != UNX_NONE && (m.classFlags & (MATERIAL_ANISOTROPIC | MATERIAL_EYE)) == 0 &&
        materialClass(m) != MATERIAL_CUT && materialClass(m) != MATERIAL_TERRAIN && loadMaterialInputs(m.inputs).heightTexture != UNX_NONE)
    {
        Texture2D<uint> classWords = ResourceDescriptorHeap[P[9].y];
        sunVisibility *= (classWords[pixel] & 0xFFu) / 255.0;
    }
    if (P[8].w != UNX_NONE)
    {
        // W stage 2 (v1.77): under water from the sun, the sun arrives along the refracted direction, attenuated by the
        // surface's transmission and the water's absorption (VSM visibility stays the unrefracted direction's, W's condition)
        float3 lw, tw;
        if (waterSunLight(P[8].x, P[8].y, P[8].z, P[8].w, P[9].x, worldPos, l0, 0, lw, tw))  // P[9].x: W's caustics
        {
            l0 = lw;
            E *= tw;
        }
    }
    const float NoL = dot(n, l0);
    if (!AREA_LOBES && sunVisibility > 0 && (experiment & 16) == 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));  // L_sun x solid angle of the disk
        // The disk's parts above and below the shading normal's horizon (cap-averaged clipped cosines).
        const float above = shCapCosine(NoL), below = shCapCosine(-NoL);
        float3 sun = 0, coatSun = 0;
        if (NoV > 0)
        {
            if (above > 0)
            {
#if SUBSURFACE
                const float e = modelDirectionalAlbedo(NoV, skin.roughness);  // (the compensation at the lobes' average roughness)
#else
                const float e = modelDirectionalAlbedo(NoV, s.roughness);
#endif
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap;
                // lobe 0: the base's specular; A9 lobe 1: the coat's (F = 1 through the same disk rules, then the exact
                // dielectric Fresnel and A2's scale at the disk centre, or for a lobe narrower than the disk its albedo
                // E_ms(n.v) over the single-scattering E(n.v)): one inlined disk integral for both
                uint lobes = 1;
#if LAYERED == 1
                if (cover > 0) lobes = 2;
#endif
#if SUBSURFACE
                if (skin.mix < 1) lobes = 2;  // (the Subsurface class's lobes 0 and 1 through the same disk integral)
#endif
                [loop] for (uint lobe = 0; lobe < lobes; ++lobe)
                {
                    const bool coatLobe = lobe == 1;
#if LAYERED == 1
                    const float3 lf0 = coatLobe ? 1.0.xxx : f0, lcomp = coatLobe ? 1.0.xxx : compensation;
                    const float lr = coatLobe ? coat.roughness : s.roughness;
#elif SUBSURFACE
                    const float3 lf0 = f0, lcomp = compensation;
                    const float lr = lobe == 1 ? skin.roughness1 : skin.roughness0;
#else
                    const float3 lf0 = f0, lcomp = compensation;
                    const float lr = s.roughness;
#endif
                    const float la = modelAlpha(lr);
                    float3 spec = (experiment & 1) ? (NoL > 0 ? shSpecular(lf0, la, lcomp, n, v, l0, NoV, NoL) * NoL * cap : 0)
                                                   : shSunSpecular(lf0, lr, la, lcomp, n, v, NoV, l0, E, shPixelAngle(D, Dx));
#if LAYERED
                    if (aniso.on && !coatLobe) spec = shAnisoSunSpecular(aniso, f0, n, v, NoV, l0, E, shPixelAngle(D, Dx));
#endif
#if LAYERED == 1
                    if (coatLobe)
                    {
                        coatSun = spec * shCoatSunWeight(coat, v, l0, NoV, NoL, (experiment & 1) != 0);
                        continue;
                    }
#endif
#if SUBSURFACE
                    sun += spec * (lobe == 1 ? 1 - skin.mix : skin.mix);
#elif LAYERED == 2
                    sun += spec * keepCloth;
#else
                    sun += spec;
#endif
                }
            }
            if (foliage) sun += back * below * cap;
#if SUBSURFACE
            // the sun through a thin part: the disk's part below the horizon is the light's cosine on the far side
            if (s.transmission > 0 && below > 0) sun += thin * (modelSubsurfaceThin(below, v, l0) * cap);
#endif
        }
        else if (foliage) sun = back * above * cap;  // viewer behind the shading normal: only light crossing the leaf
#if SUBSURFACE
        else if (s.transmission > 0 && above > 0) sun = thin * (modelSubsurfaceThin(above, v, l0) * cap);
#endif
#if LAYERED == 1
        // the coat lobe over the disk; the base through the coat at the disk centre (its lobe is widened by the coat)
        if (cover > 0 && NoV > 0)
            sun = keep * sun + cover * (coatSun + (NoL > 0 ? modelCoatUnder(s, coat, n, v, l0) * above * cap : 0));
#endif
#if LAYERED == 2
        if (NoV > 0)
        {
            // the sheen lobe over the disk by the 4-point rule (MATERIAL_LAYERS 1.4: 1.5e-3 against the disk average)
            sun = keepS * sun + sheen.color * (modelSheenSun(sheen.roughness, n, v, l0, g_sunAngularRadius) * cap);
        }
#endif
#if SUBSURFACE
        // an eye's iris: the sun on the iris plane (at the disk's centre) in place of the cornea's surface
        const float irisSun = eye.mask > 0 && NoV > 0 ? modelEyeCosine(eye, above, l0) - above : 0;
        sun += front * (irisSun * cap);
#endif
#if SSS_SPLIT
        // the sun's diffuse light per unit f_d: the disk's cosine on the viewer's side and its light through a thin part
        // (W is 0 where the disk does not reach the far side)
        scatterE += cap * ((NoV > 0 ? above + s.transmission * modelSubsurfaceThin(below, v, l0) : s.transmission * modelSubsurfaceThin(above, v, l0)) * sunVisibility);
        scatterE += cap * (irisSun * sunVisibility);
#endif
        radiance += sun * sunVisibility;
    }
#endif
#if (SHADE_PART == 1 || SHADE_PART == 4)

#if SHADE_PART != 4
    // ---- local lights (main view: S's froxel lists)
    FroxelSrvs froxels;
    froxels.lights = P[5].x;
    froxels.lightIndices = P[5].x;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    // L2 (14.1/14.2) tile record state: the FAR lights' diffuse comes once from the tile term after the loop.
    bool tileFar = false;
    uint2 nearMask = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    TileLightRecord tileRec = (TileLightRecord)0;
#if MEGA_LIGHTS
    // the pixel's light samples (MegaLightsUpsample.hlsli) and what their lights add, diffuse and specular apart
    const MlPixelLights mls = mlPixelLights(P[10].y, P[10].z, pixel, worldPos, n, linearZ, P[11].w & 0xFFu, (P[11].w >> 8) & 0xFFu,
                                            f16tof32(P[11].y & 0xFFFFu), f16tof32(P[11].y >> 16));
    float3 mlDiffuse = 0, mlSpecular = 0;
#endif
    // (shading.mega_lights: P[10].y carries the stochastic result and the full kernels skip the loop; the A9 lobe kernel
    // keeps S's visibility)
    if (froxels.lights != UNX_NONE && (experiment & 32) == 0 && (AREA_LOBES || MEGA_LIGHTS || P[10].y == UNX_NONE))  // this view's lists (v1.22)
    {
        uint shadowPacked = 0xFFFFFFFFu;  // all slots lit when S publishes no visibility
        if (!MEGA_LIGHTS && P[2].x != UNX_NONE && (experiment & 2048) == 0)
        {
            Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
            shadowPacked = shadow[pixel];
        }
#if SUBSURFACE
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), skin.roughness);  // (the lobes' average roughness)
#else
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
#endif
        const float3 compensation = 1 + f0 * (1 / e - 1);
#if MEGA_LIGHTS
        const uint2 range = uint2(0, mls.count);
        const uint indexBase = 0;
#else
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        const uint indexBase = froxelIndexBase(froxels);
#if (SHADE_PART == 1 || SHADE_PART == 4)
        // lighting channels: the receiver's are its instance's (the pixel's vis id), for every light of the loop below
        if (P[11].z != UNX_NONE)
        {
            Texture2D<uint> channelVis = ResourceDescriptorHeap[P[11].z];
            const uint channelVisId = channelVis[pixel];
            if (channelVisId != VIS_NONE)
                g_lightChannels = instanceLightingChannels(loadInstance(loadVisibleCluster(P[11].w, visVisibleCluster(channelVisId)).instance).flags);
        }
#endif
#endif
        uint shadowOrdinal = 0, overflowRecord = 0xFFFFFFFFu;
        uint2 overflowPacked = uint2(0xFFFFFFFFu, 0);
#if !FALLBACK && !MEGA_LIGHTS
        // Resolve the pixel record before LTC/BRDF work. Visibility words are
        // still demand-loaded, at most once for each four consecutive casters.
        overflowRecord = 0;
        if (range.y > 3 && overflowHead != 0 && P[7].z != UNX_NONE)
        {
            ByteAddressBuffer records = ResourceDescriptorHeap[P[7].z];
            overflowRecord = records.Load(4 * (overflowHead - 1 + (pixel.y % M_TILE) * M_TILE + (pixel.x % M_TILE)));
        }
#endif

#if AREA
        // Area lights (AreaLight.hlsli): the shading frame, its horizon-flipped twin for Foliage transmission and the
        // LTC transform of the specular lobe, formed once per pixel.
        const float3x3 frame = shShadingFrame(n, v, NoV);
        const float3x3 frameBack = float3x3(frame[0], -frame[1], -frame[2]);
#if SUBSURFACE
        // the Subsurface class's two lobes over an area light: each lobe's LTC and albedo (a Standard lobe at its
        // roughness), weighed by the mix
        float3x3 specular = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), skin.roughness0), frame);
        float3 specularAlbedo = skin.mix * shSpecularAlbedo(f0, max(NoV, 1e-4), skin.roughness0);
        const float3x3 specular1 = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), skin.roughness1), frame);
        const float3 specularAlbedo1 = (1 - skin.mix) * shSpecularAlbedo(f0, max(NoV, 1e-4), skin.roughness1);
        const float3x3 frameIris = shShadingFrame(eye.iris, v, dot(eye.iris, v));  // (an eye's iris plane; unused without one)
#else
        float3x3 specular = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), s.roughness), frame);
        float3 specularAlbedo = shSpecularAlbedo(f0, max(NoV, 1e-4), s.roughness);
#endif
#if LAYERED == 2
        specularAlbedo *= keepCloth;  // (the cloth blend)
#endif
#if LAYERED == 1
        float3x3 coatSpecular = frame, coatBase = frame;
        float coatAlbedo = 0;
        float3 coatBaseAlbedo = 0;
        if (cover > 0 && NoV > 0)
        {
            const float rEq = modelCoatBaseRoughness(s, coat, NoV);
            coatSpecular = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), coat.roughness), frame);
            coatBase = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), rEq), frame);
            coatAlbedo = modelCoatEms(coat, NoV);
            coatBaseAlbedo = shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), rEq);
        }
#endif
#endif
        ShadowPixelReceiver overflowReceiver = (ShadowPixelReceiver)0;
        uint4 lightWords = 0;
        // L3 (14.3-2): S's tile classification: an entry lit over every pixel of the tile has visibility 1 here, without
        // the slot or overflow read (shadow.vsm.classification_pages; P[10].w, UNX_NONE: off. Until 2026-10-01 the kernel
        // read P[6].z, the exposure histogram's centre weight in the record: never S's buffer).
        VsmClsTile clsTile = (VsmClsTile)0;
        uint clsSlice = 0;
#if !FALLBACK && !MEGA_LIGHTS  // (fallback tiles keep the full read path: the DXIL limit)
        if (P[10].w != UNX_NONE)
        {
            clsTile = vsmClsTile(P[10].w, (pixel.y / M_TILE) * ((g_viewWidth + M_TILE - 1) / M_TILE) + pixel.x / M_TILE);
            clsSlice = froxelSlice(froxelGrid(froxels.lights), linearZ);
        }
#endif
        // L2 (14.1/14.2): this tile's record: FAR lights (a clear bit of the pixel's slice mask) skip their diffuse term
        // here and come back as the tile corners' vector irradiance below; Foliage keeps every light per pixel.
#if !FALLBACK && !MEGA_LIGHTS  // (fallback tiles - S's overflow - keep every light per pixel: the record is an optimisation, not a value)
        if (P[11].x != UNX_NONE && !foliage)
        {
            tileRec = tileLightsRecord(P[11].x, (pixel.y / M_TILE) * ((g_viewWidth + M_TILE - 1) / M_TILE) + pixel.x / M_TILE);
            if (tileLightsValid(tileRec))
            {
                const uint slice = froxelSlice(froxelGrid(froxels.lights), linearZ), rel = slice - tileLightsFirstSlice(tileRec);
                if (rel < tileLightsSliceCount(tileRec))
                {
                    tileFar = true;
                    nearMask = tileRec.mask[rel];
                }
            }
        }
#endif
        for (uint i = 0; i < range.y; ++i)
        {
#if MEGA_LIGHTS
            const uint lightIndex = mls.light[i];
            const bool isFar = false;
            if (!(mls.weight[i] > 0)) continue;
#else
            const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
            const bool isFar = tileFar && i < 64 && (((i < 32 ? nearMask.x : nearMask.y) >> (i & 31)) & 1u) == 0;
#endif
            const float3 frontL = isFar ? 0 : front;  // (L2: the FAR light's diffuse is in the tile term)
            const GpuLight light = loadLight(lightIndex);
            // The light's shadow ordinal is counted here; its visibility (a slot, or the overflow records' dependent
            // loads) is read only for a light that adds something at this pixel (a window, a spot factor and a lobe on
            // the side it lights): the others added exactly 0.
            const bool casts = lightCastsShadow(light);
            if (casts) ++shadowOrdinal;
            // diagnostic 16384 (R's request 2026-10-01, the 128-slot limit's share): a shadow-casting light that has no VSM slot
            // this frame (froxel entry bit 15 clear: S lit it unshadowed) adds nothing; L3 stage 5 removes the limit
#if !MEGA_LIGHTS
            if ((experiment & 16384) != 0 && casts && (froxelEntryAt(froxels, indexBase, range.x + i) & 0x8000u) == 0) continue;
#endif
            const bool area = lightType(light) > LIGHT_SPOT;
            float3 p = 0, toLight = 0, l = 0, E = 0;
            float window = 0, cosL = 0;
            if (area)
            {
#if AREA
                // Lights whose window is 0 here (the froxel's lists hold every light reaching the froxel) add exactly 0.
                p = (light.position - g_cameraPosition) - offset;
                window = shAreaWindow(light, p);
                if (window <= 0) continue;
#else
                continue;  // AREA=0: the scene has no area lights (ShadingSystem)
#endif
            }
            else
            {
#if AREA_LOBES
                continue;  // (punctual lights: the full kernel)
#endif
                toLight = (light.position - g_cameraPosition) - offset;
                E = shPunctualIlluminance(light, toLight, l);
                cosL = dot(n, l);
                // Every lobe below is 0 off (NoV > 0, cosL > 0) and the Foliage back side (the coat and sheen lobes too:
                // MaterialModel.hlsli returns 0 for NoL <= 0), and E = 0 outside the window or the spot cone.
#if SUBSURFACE
                // (the Subsurface class: the far side adds the light through thin parts)
                // (an eye's iris takes the light on its plane, whatever the cornea's cosine)
                if (all(E == 0) || !((NoV > 0 && cosL > 0) || (s.transmission > 0 && NoV * cosL < 0) || (eye.mask > 0 && NoV > 0))) continue;
#else
                if (all(E == 0) || !((NoV > 0 && cosL > 0) || (foliage && NoV * cosL < 0))) continue;
#endif
            }
            float visibility = 1;
#if MEGA_LIGHTS
            visibility = mls.weight[i];  // the samples' weight: 1 / probability x visible fraction (MegaLightsUpsample.hlsli)
            if (false)
#elif FALLBACK
            if (casts)
#else
            if (casts && P[10].w != UNX_NONE && vsmClsTileUmbra(clsTile, clsSlice, i)) continue;  // umbra over the tile: 0
            if (casts && !(P[10].w != UNX_NONE && vsmClsTileLit(clsTile, clsSlice, i)))
#endif
                visibility = shadowOrdinal <= 3 ? shadowSlot(shadowPacked, shadowOrdinal)
                                                : shOverflowVisibility(pixel, shadowOrdinal, overflowHead, overflowRecord, overflowPacked, overflowReceiver, lightIndex);
            if (visibility <= 0) continue;
            if (area)
            {
#if AREA
                // L w (f_d pi I + E_s I_ltc) on the viewer's side of n; Foliage transmits what arrives on the other.
                const float3 Lw = shAreaColor(light, p) * (light.intensity * window * visibility);  // (a rect's source texture: AreaLight.hlsli)
#if AREA_LOBES || (MEGA_LIGHTS && LAYERED)
                // A9 the lobes no LTC represents, over the light (AreaQuadrature.hlsli): the anisotropic base (MATERIAL_LAYERS
                // 1.5; under a coat scaled like the base) and the sheen (1.4) - on the viewer's side. The lobe kernel
                // (AREA_LOBES) holds these alone, with S's visibility; shading.mega_lights' kernel adds them to its specular
                // with the light samples' weight (Lw), and the lobe kernel then does not run (ShadingSystem.cpp).
                if (NoV > 0)
                {
                    float3 lobe = 0;
#if LAYERED == 1
                    if (aniso.on && !shLightSpecularInResult(lightIndex))
                        lobe = ((cover > 0) ? keep : 1.0) * Lw * lightSpecularScale(light) *
                               shAreaAniso(light, p, aniso.t, aniso.b, n, v, aniso.alpha, f0, 1 + f0 * (1 / (aniso.ab.x + aniso.ab.y) - 1));
#endif
#if LAYERED == 2
                    lobe = Lw * lightSpecularScale(light) * sheen.color * shAreaSheen(light, p, frame, v, sheen.roughness);
#endif
#if MEGA_LIGHTS
                    mlSpecular += lobe;
#else
                    radiance += lobe;
#endif
                }
#if AREA_LOBES
                continue;
#endif
#endif
                // Integrals in order: front diffuse, specular, back (Foliage) -- one inlined evaluator (the diffuse frames
                // are rotations: closed forms on circular cones).
                uint first = NoV > 0 ? 0 : 2, last = foliage ? 3 : 2;
                const bool specularInReflections = shLightSpecularInResult(lightIndex);  // P[4].x: B2 mask (planar views: none)
                float scaleBase = 1;
#if LAYERED == 2
                scaleBase = keepS;  // (the sheen lobe over area lights: the lobe texture, AreaLobes.hlsl)
#endif
#if SUBSURFACE
                // integral 2: the light through a thin part (the far side's cosine); 3: the Subsurface class's lobe 1
                // (integral 1 is its lobe 0); 4: an eye's iris plane
                if (s.transmission > 0) last = 3;
                if (NoV > 0 && skin.mix < 1) last = 4;
                if (NoV > 0 && eye.mask > 0) last = 5;
                float irisI0 = 0;
#endif
#if LAYERED == 1
                // A9 (MATERIAL_LAYERS 3.1): integrals 3 (the coat lobe, its own LTC, albedo E_ms(n.v)) and 4 (the base
                // lobe through the coat, the LTC of the outside-equivalent roughness alpha_eq ~ eta alpha'_b) in the same
                // loop; the base's diffuse-like part through the coat reuses integral 0, with the coat's transmission
                // and the returned light at the light's centre direction.
                float3 coatAdd = 0;
                float coatId = 0, tvtl = 0;
                if (cover > 0 && NoV > 0)
                {
                    scaleBase = keep;
                    last = 5;
                    const float muL = max(dot(n, normalize(p)), 1e-4);
                    tvtl = (1 - modelCoatEms(coat, NoV)) * (1 - modelCoatEms(coat, muL)) / (coat.eta * coat.eta);
                }
#endif
                [loop] for (uint j = first; j < last; ++j)
                {
#if SUBSURFACE
                    if ((j == 1 || j == 3) && specularInReflections) continue;
                    if ((j == 2 && !(s.transmission > 0)) || (j == 3 && !(skin.mix < 1))) continue;
#else
                    if ((j == 1 || j >= 3) && specularInReflections) continue;
#endif
#if LAYERED
                    if (j == 1 && aniso.on) continue;  // (the anisotropic lobe over the light: the lobe texture, AreaLobes.hlsl)
#endif
#if !SUBSURFACE
                    if (j == 2 && !foliage) continue;
#endif
#if LAYERED == 1
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (j == 3 ? coatSpecular : (j == 4 ? coatBase : (NoV > 0 ? frameBack : frame))));
#elif SUBSURFACE
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (j == 3 ? specular1 : (j == 4 ? frameIris : (NoV > 0 ? frameBack : frame))));
#else
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (NoV > 0 ? frameBack : frame));
#endif
#if SUBSURFACE
                    const float I = shAreaIntegral(light, p, T, j == 0 || j == 2 || j == 4);
#else
                    const float I = shAreaIntegral(light, p, T, j == 0 || j == 2);
#endif
#if SUBSURFACE
                    if (j == 0) irisI0 = I;
                    if (j == 3)
                    {
#if MEGA_LIGHTS
                        mlSpecular += Lw * (specularAlbedo1 * I);
#else
                        radiance += Lw * (specularAlbedo1 * I);
#endif
                        continue;
                    }
                    if (j == 2 || j == 4)
                    {
                        // 2: the light through a thin part - the far side's cosine integral, W over the cosine at the
                        //    light's centre (the cosine held above 1/16: W / c grows without bound at the horizon);
                        // 4: an eye's iris - the plane's cosine integral with the caustic at the light's centre, in place
                        //    of the surface's (integral 0) on the mask's share.
                        const float3 lc = normalize(p);
                        const float c0 = max(abs(dot(n, lc)), 0.0625);
                        const float Ed = j == 2 ? s.transmission * (SH_PI * I * modelSubsurfaceThin(c0, v, lc) / c0)
                                                : eye.mask * (SH_PI * (I * modelEyeCaustic(eye, lc) - irisI0));
#if SSS_SPLIT
                        scatterE += Lw * Ed;
#elif MEGA_LIGHTS
                        mlDiffuse += Lw * (diffuse * Ed);
#else
                        radiance += Lw * (diffuse * Ed);
#endif
                        continue;
                    }
#endif
#if LAYERED == 1
                    if (j == 0) coatId = I;
                    if (j >= 3)
                    {
                        coatAdd += (j == 3 ? coatAlbedo : tvtl * coatBaseAlbedo) * I;
                        continue;
                    }
#endif
#if MEGA_LIGHTS
                    if (j == 1) mlSpecular += scaleBase * Lw * (specularAlbedo * I);
                    else mlDiffuse += scaleBase * Lw * ((j == 0 ? frontL : back) * (SH_PI * I));
#else
                    radiance += scaleBase * Lw * (j == 0 ? frontL * (SH_PI * I) : (j == 1 ? specularAlbedo * I : back * (SH_PI * I)));
#endif
#if SSS_SPLIT
                    if (j == 0 && !isFar) scatterE += Lw * (SH_PI * I);  // (the light's diffuse per unit f_d; a FAR light's: the tile term)
#endif
                }
#if LAYERED == 1
                if (last == 5)
                {
                    const float muIn = modelCoatRefractedCos(max(dot(n, normalize(p)), 1e-4), coat.eta);
                    coatAdd += tvtl * (diffuse + modelCoatReturned(s, coat, muIn) / SH_PI) * (SH_PI * coatId);
#if MEGA_LIGHTS
                    mlSpecular += cover * Lw * coatAdd;
#else
                    radiance += cover * Lw * coatAdd;
#endif
                }
#endif
#endif
                continue;  // AREA=0: the scene has no area lights (ShadingSystem)
            }
#if AREA_LOBES
            continue;  // (punctual lights: the full kernel)
#endif
            if (P[6].w != UNX_NONE)
            {
                // A8 (E's table): the light's function in the direction from the light to this point; the footprint is
                // the pixel's size seen from the light (cookie and gobo mip selection)
                const float footprint = linearZ * (2 * g_tanHalfFovY / g_viewHeight) / max(length(toLight), 1e-4);
                E *= lightFunction(P[6].w, lightIndex, light.forward, light.right, -l, footprint, g_time);
            }
            float3 f = 0;
#if SUBSURFACE
            if (NoV > 0 && cosL > 0) f = frontL + shSpecularSubsurface(f0, skin, compensation, n, v, l, NoV, cosL);
            else if (NoV * cosL < 0) f = thin * (modelSubsurfaceThin(abs(cosL), v, l) / abs(cosL));  // (transmission > 0: the test above)
#else
            if (NoV > 0 && cosL > 0) f = frontL + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
#endif
#if LAYERED
            if (aniso.on && NoV > 0 && cosL > 0) f = frontL + shAnisoSpecular(aniso, f0, n, v, l) * shLightSpecular();  // (as the lobe it replaces)
#endif
#if LAYERED == 2
            if (NoV > 0 && cosL > 0) f -= (f - frontL) * sheen.cloth;  // (the cloth blend: the base's specular lobe x (1 - cloth))
#endif
#if LAYERED == 1
            // (the coat's and the sheen's lobes take the light's specular scale: ShadingCommon.hlsli shLightSpecular)
            if (cover > 0) f = keep * f + cover * (modelCoatLobe(coat, n, v, l) * shLightSpecular() + modelCoatUnder(s, coat, n, v, l));
#endif
#if LAYERED == 2
            if (NoV > 0 && cosL > 0) f = keepS * f + sheen.color * (modelSheenLobe(sheen.roughness, n, v, l) * shLightSpecular());
#endif
#if MEGA_LIGHTS
            {
                // the diffuse share of f (the rest is specular: base lobe, coat, sheen), and the smooth cut under the
                // minimum sample weight the sampling used
                float3 fd = 0;
                if (NoV > 0 && cosL > 0) fd = frontL;
                else if (foliage && NoV * cosL < 0) fd = back;
#if SUBSURFACE
                else if (NoV * cosL < 0) fd = f;  // (the light through a thin part: diffuse)
#endif
#if LAYERED == 1
                if (cover > 0) fd *= keep;
#endif
#if LAYERED == 2
                if (NoV > 0 && cosL > 0) fd *= keepS;
#endif
                const float3 unshadowed = E * abs(cosL);
                const float cw = visibility * mlFalloffMask(mlLuminance(f * unshadowed) * g_exposure, asfloat(P[11].z));
                const float3 c = unshadowed * cw;
                mlDiffuse += fd * c;
                mlSpecular += max(f - fd, 0.0) * c;
#if SUBSURFACE
                // an eye's iris: the light on the iris plane in place of the cornea's surface (modelEyeCosine)
                if (eye.mask > 0 && NoV > 0) mlDiffuse += frontL * (E * ((modelEyeCosine(eye, cosL, l) - max(cosL, 0.0)) * cw));
#endif
            }
#else
            radiance += f * E * (abs(cosL) * visibility);
#if SUBSURFACE
            // an eye's iris: the light on the iris plane in place of the cornea's surface (modelEyeCosine); per unit f_d
            // with SSS_SPLIT (front is 0 there)
            if (eye.mask > 0 && NoV > 0)
            {
                const float irisE = (modelEyeCosine(eye, cosL, l) - max(cosL, 0.0)) * visibility;
                radiance += frontL * (E * irisE);
#if SSS_SPLIT
                scatterE += E * irisE;
#endif
            }
#endif
#endif
#if SSS_SPLIT
            // the light's diffuse per unit f_d (a FAR light's: the tile term), or its light through a thin part
            if (NoV > 0 && cosL > 0)
            {
                if (!isFar) scatterE += E * (cosL * visibility);
            }
            else if (NoV * cosL < 0) scatterE += E * (s.transmission * modelSubsurfaceThin(abs(cosL), v, l) * visibility);
#endif
        }
    }
#if MEGA_LIGHTS
    {
        // exposed and divided by the modulation factors (MegaLights.hlsli; m.ml.spatial multiplies them back), within f16
        RWTexture2D<float4> outDiffuse = ResourceDescriptorHeap[P[10].w];
        RWTexture2D<float4> outSpecular = ResourceDescriptorHeap[P[11].x];
        const float3 fD = mlDiffuseFactor(s.baseColor, s.metallic), fS = mlSpecularFactor(s.baseColor, s.metallic, s.specular, s.roughness, NoV);
        outDiffuse[pixel] = float4(min(mlDiffuse * g_exposure / fD, 60000.0), mls.confidence);
        outSpecular[pixel] = float4(min(mlSpecular * g_exposure / fS, 60000.0), mls.valid ? 1 : 0);
    }
#endif

    // L2 (14.2): the FAR lights' diffuse, once: the tile corners' vector irradiance (every FAR light above the tile's
    // normal cone with margin, so n . E is their exact sum up to the 1e-3 interpolation rule), bilinear in the tile.
#if SSS_SPLIT && !FALLBACK
    if (tileFar && NoV > 0) scatterE += max(0.0, dot(n, tileLightsIrradiance(tileRec, pixel & (M_TILE - 1))));
#elif !AREA_LOBES && !FALLBACK
    if (tileFar && NoV > 0) radiance += front * max(0.0, dot(n, tileLightsIrradiance(tileRec, pixel & (M_TILE - 1))));
#endif

#endif  // SHADE_PART != 4: local lights already supplied by MegaLights

    // 14.1b (L2b): the converted emissive surfaces as area lights - their diffuse irradiance on the viewer's side of n
    // (EmissiveDirect.hlsl: quadtree nodes as horizon-clipped Lambert polygons; the specular side is the reflection
    // path's, which sees the emissive geometry: B2). Node shadows: 14.3 (L3). Foliage's back side: not yet.
#if !AREA_LOBES && !MEGA_LIGHTS
    if (P[10].z != UNX_NONE && NoV > 0)
    {
        Texture2D<float4> emissiveE = ResourceDescriptorHeap[P[10].z];
#if SSS_SPLIT
        scatterE += emissiveE[pixel].rgb / g_exposure;
#else
        radiance += front * (emissiveE[pixel].rgb / g_exposure);
#endif
    }
#endif
#endif  // direct light

#if SHADE_PART == 4
    // Same addition boundary as part 2, after part 1's float32 direct radiance.
    Texture2D<float4> megaLights = ResourceDescriptorHeap[P[10].y];
    radiance += megaLights[pixel].rgb / g_exposure;
#endif

#if AREA_LOBES
    {
        RWTexture2D<float4> lobes = ResourceDescriptorHeap[P[9].z];
        lobes[pixel] = float4(radiance * g_exposure, 0);  // (exposed: f16 range)
    }
#elif SHADE_PART == 2 || SHADE_PART == 4
    // ---- indirect (R): screen probes (main view) or the world cache (planar views). The viewer's side of the shading
    // normal reflects; Foliage also transmits what arrives on the other side.
    const float3 nv = NoV > 0 ? n : -n;
    // the base lobe's albedo for the indirect specular (A9 anisotropy: f0 A_a + B_a of its lobe; the cone is the equal-area one)
    float3 baseAlbedo = shSpecularAlbedo(f0, NoV, s.roughness);
#if LAYERED
    if (aniso.on) baseAlbedo = shAnisoAlbedo(aniso, f0);
#endif
    const float3 r = reflect(-v, n);
    const float halfAngle = reflectionLobeHalfAngle(s.roughness, NoV);
    float3 irradiance = 0, irradianceBack = 0, incident = 0, coatIncident = 0;
#if !PLANAR
    if (P[2].y != UNX_NONE && (experiment & 6) != 6)
    {
        // One probe footprint for the irradiance (both sides for Foliage) and, where R's reflection has no G/M result,
        // the K-path radiance of the lobe (R's hardware-filtered maps); records from the tile cache loaded in main.
        ProbeSrvs probes;
        probes.probes = P[2].y;
        probes.occlusion = P[2].y;
        probes.pad0 = P[7].y;
        // R's per-pixel front irradiance (P[9].w, view.giIrradiance: r.gi.screen, then R's edge-preserving filter, with the
        // probes' near occlusion multiplied in): where it has data it is the pixel's whole front diffuse indirect
        // irradiance, and the gather runs only for what the texture does not hold - the K path's radiance and Foliage's
        // back side (its occlusion from the footprint as before; the cache stays for that side). Elsewhere (no texture, or
        // no data at the pixel) the gather's irradiance x occlusion as before.
        float4 screenE = 0;
        if (P[9].w != UNX_NONE)
        {
            Texture2D<float4> screenIrradiance = ResourceDescriptorHeap[P[9].w];
            screenE = screenIrradiance[pixel];  // rgb = irradiance x occlusion x exposure, a = 1 where the cache had data
        }
        const bool fromScreen = screenE.a > 0;
        const bool gatherIrradiance = (experiment & 2) == 0 && !fromScreen;
        probes.pad1 = P[2].w != UNX_NONE && (gatherIrradiance || foliage) ? P[2].w + 1 : 0;
        const bool specular = NoV > 0 && (experiment & 4) == 0;
        const float4 refl = specular && P[2].z != UNX_NONE ? reflectionRadiance(P[2].z, pixel) : float4(0, 0, 0, 0);
        // (refl.a: the traced reflection's share of the lobe radiance - 1 or 0, in between over reflection.lumen's
        // roughness fade, where the K path's radiance makes up the rest)
        const bool wantRadiance = specular && refl.a < 1;
        if (fromScreen && (experiment & 2) == 0) irradiance = screenE.rgb / g_exposure;
        // gather 0: the pixel's irradiance and its lobe's K-path radiance; A9 gather 1: the coat lobe's cone (K path; R's
        // reflection result is the base lobe's) - one inlined gather for both
        uint gathers = 1;
#if LAYERED == 1
        if (cover > 0 && specular) gathers = 2;
#endif
        [loop] for (uint gi = 0; gi < gathers; ++gi)
        {
            const bool coatCone = gi == 1;
            const bool backSide = !coatCone && foliage && (experiment & 2) == 0;
            if (!coatCone && !(gatherIrradiance || backSide || wantRadiance)) continue;
#if LAYERED == 1
            const float cone = coatCone ? reflectionLobeHalfAngle(coat.roughness, NoV) : halfAngle;
#else
            const float cone = halfAngle;
#endif
            const ScreenProbeLighting g = screenProbeGatherTile(probes, pixel / M_TILE, pixel, worldPos, nv, linearZ, backSide, coatCone || wantRadiance, r, cone,
                                                                !coatCone && gatherIrradiance);
            if (coatCone)
            {
                coatIncident = g.radiance;
                continue;
            }
            if (gatherIrradiance) irradiance = g.irradiance * g.occlusion;
            if (backSide) irradianceBack = g.irradianceBack * g.occlusion;
            if (wantRadiance) incident = g.radiance;
        }
        if (specular && refl.a > 0) incident = refl.a >= 1 ? refl.rgb : lerp(incident, refl.rgb, refl.a);
    }
    else if (P[9].w != UNX_NONE && (experiment & 6) != 6)
    {
        // gi.lumen_only: the final gather's outputs, composed as the reference composes them (ue6-main
        // DiffuseIndirectComposite.usf, ClearCoatCommon.ush read; the code is ours):
        //   diffuse    view.giIrradiance x the short-range AO with its multi-bounce rescale (albedo held to 0.5);
        //   specular   the gather's rough specular x the specular occlusion, and R's reflection over it by the
        //              reflection's share (1 where traced or planar, the roughness fade between) - the occlusion is on
        //              the rough part alone;
        //   clearcoat  the reflection is the coat's (R traces a coated pixel at its coat's roughness: the top layer);
        //              the base lobe takes the rough specular (the bottom layer);
        //   Foliage    the back side from the gather's backface irradiance (the screen probes at the reversed normal:
        //              the reference's backface diffuse), or without it the translucency volume (P[2].w).
        Texture2D<float4> diffuseIndirect = ResourceDescriptorHeap[P[9].w];
        float4 bent = lumenShortRangeAO(P[11].z, pixel, nv);
        // The material's baked occlusion (cavities under the screen's resolution or deeper than the short-range search;
        // the reference's G-buffer AO): the indirect light takes the smaller of the two, the traced reflection the
        // specular occlusion of the baked value alone (the screen's occluders are in its rays).
        const float materialAo = mWordOcclusion(word, m.classFlags);
        bent.w = min(bent.w, materialAo);
        if ((experiment & 2) == 0)
        {
            irradiance = max(diffuseIndirect[pixel].rgb, 0.0) / g_exposure * lumenAoMultibounce(s.baseColor * (1 - s.metallic), bent.w, 0.5);
#if SUBSURFACE
            if (eye.mask > 0 && giSourceIsVolume(P[2].w))
            {
                // an eye's iris (see the header): the gather's irradiance turned from the surface normal to the iris plane
                const LtvSh sh = ltvSample(ltvParams(giSourceVolume(P[2].w)), worldPos);
                const float3 eN = ltvIrradianceOf(sh, nv), eA = ltvIrradianceOf(sh, eye.iris);
                irradiance *= lerp(1.0, clamp(eA / max(eN, 1e-6), 0.25, 4.0), eye.mask);
            }
#endif
            if (foliage && P[11].w != UNX_NONE)
            {
                Texture2D<float4> backfaceIndirect = ResourceDescriptorHeap[P[11].w];
                irradianceBack = max(backfaceIndirect[pixel].rgb, 0.0) / g_exposure;
            }
            else if (foliage && giSourceIsVolume(P[2].w)) irradianceBack = ltvIrradiance(giSourceVolume(P[2].w), worldPos, -nv);
        }
        if (NoV > 0 && (experiment & 4) == 0)
        {
            float3 rough = 0;
            if (P[11].y != UNX_NONE)
            {
                Texture2D<float4> roughSpecular = ResourceDescriptorHeap[P[11].y];
                rough = max(roughSpecular[pixel].rgb, 0.0) / g_exposure * lumenAoSpecular(n, s.roughness, bent.w, v, bent.xyz * bent.w);
            }
            float4 refl = P[2].z != UNX_NONE ? reflectionRadiance(P[2].z, pixel) : float4(0, 0, 0, 0);
            if (materialAo < 1) refl.rgb *= mSpecularOcclusion(NoV, s.roughness, materialAo);
            incident = lerp(rough, refl.rgb, saturate(refl.a));
#if LAYERED == 1
            if (cover > 0)
            {
                coatIncident = incident;
                incident = rough;
            }
#endif
        }
    }
#else
    if (giSourceIsVolume(P[2].w))
    {
        // gi.lumen_only: the main view's translucency volume at the surface point (a point the main view does not see
        // takes its nearest cell): irradiance on each side, the lobes' radiance from the mirror direction (two SH bands)
        const LtvSh sh = ltvSample(ltvParams(giSourceVolume(P[2].w)), worldPos);
        irradiance = ltvIrradianceOf(sh, nv);
#if SUBSURFACE
        if (eye.mask > 0) irradiance = lerp(irradiance, ltvIrradianceOf(sh, eye.iris), eye.mask);  // (an eye's iris: its plane's)
#endif
        if (foliage) irradianceBack = ltvIrradianceOf(sh, -nv);
        if (NoV > 0) incident = ltvRadianceOf(sh, r);
#if LAYERED == 1
        if (NoV > 0 && cover > 0) coatIncident = incident;
#endif
    }
    else if (P[2].w != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = P[2].w;
        gi.hash = P[2].w;
        gi.pad0 = gi.pad1 = 0;
        // R's per-pixel front irradiance of this view (P[9].w: r.gi.screen.planar and its filter, the main view's lookup
        // and filter), else the direct trilinear lookup
        float4 e = 0;
        if (P[9].w != UNX_NONE)
        {
            Texture2D<float4> screenIrradiance = ResourceDescriptorHeap[P[9].w];
            e = screenIrradiance[pixel];
        }
        irradiance = e.a > 0 ? e.rgb / g_exposure : giCacheIrradiance(gi, worldPos, nv);
#if SUBSURFACE
        if (eye.mask > 0) irradiance = lerp(irradiance, giCacheIrradiance(gi, worldPos, eye.iris), eye.mask);  // (an eye's iris: its plane's)
#endif
        if (foliage) irradianceBack = giCacheIrradiance(gi, worldPos, -nv);
        if (NoV > 0) incident = giCacheRadiance(gi, worldPos, n, r, halfAngle);  // looked up in this surface's normal class
#if LAYERED == 1
        if (NoV > 0 && cover > 0) coatIncident = giCacheRadiance(gi, worldPos, n, r, reflectionLobeHalfAngle(coat.roughness, NoV));
#endif
    }
#endif
    // ==== W CALL SITE (sky and GI light under water; Passes/Water/WaterLight.hlsli waterIndirectTransmittance) ====
    // Both shading parts bind the four sun-space water-map SRVs in P[8]. The indirect part's
    // gathered irradiance and reflection lobes traverse the water column before reaching this surface.
    {
        const float3 underWater = waterIndirectTransmittance(P[8].x, P[8].y, P[8].z, P[8].w, worldPos);
        irradiance *= underWater;
        irradianceBack *= underWater;
        incident *= underWater;
        coatIncident *= underWater;
    }
    // ==== end of the W call site ====
#if LAYERED == 1
    if (cover > 0 && NoV > 0)
    {
        // MATERIAL_LAYERS 3.1: the base through the coat with the coat's cosine-weighted mean transmission
        // (1 - K_ms(r_c)) eta^2 and the returned light at the cosine-weighted mean direction (mu = 2/3); the coat lobe from
        // R's probes at its own cone (K path, gather 1 above) with its albedo E_ms(n.v) - R's reflection result is the base
        // lobe's.
        const float tv = 1 - modelCoatEms(coat, NoV), tBar = 1 - modelCoatLookup1(coat.coat * MODEL_COAT_STRIDE + 4096, coat.roughness);
        const float3 under = tv * tBar * ((front + modelCoatReturned(s, coat, modelCoatRefractedCos(2.0 / 3.0, coat.eta)) / SH_PI) * irradiance +
                                          incident * shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), modelCoatBaseRoughness(s, coat, NoV)));
        radiance += keep * (front * irradiance + incident * baseAlbedo) + cover * (under + coatIncident * modelCoatEms(coat, NoV));
    }
    else
#endif
#if LAYERED == 2
    // the base's indirect light scaled; the sheen's from the irradiance: C E_sh(n.v) E / pi (exact for uniform incident
    // radiance; MATERIAL_LAYERS 1.4 states the shape error)
    // (the cloth blend: the base's specular lobe x keepCloth)
    if (NoV > 0) radiance += keepS * (front * irradiance + incident * (baseAlbedo * keepCloth)) + sheen.color * (modelSheenAlbedo(NoV, sheen.roughness) / SH_PI) * irradiance;
    else
#endif
    if (NoV > 0) radiance += front * irradiance + incident * baseAlbedo;
    radiance += back * irradianceBack;
#if SSS_SPLIT
    if (NoV > 0) scatterE += irradiance;  // (the indirect diffuse light per unit f_d)
#endif

#if LAYERED
    // A9: the area-light lobes no LTC represents (AreaLobes.hlsl, dispatched before this kernel on the same tiles;
    // area-lit scenes only: P[9].z)
    if (P[9].z != UNX_NONE)
    {
        RWTexture2D<float4> lobes = ResourceDescriptorHeap[P[9].z];
        radiance += lobes[pixel].rgb / g_exposure;
    }
#endif
#endif  // AREA_LOBES, SHADE_PART == 2 (the indirect light)
#if SHADE_PART == 3
    // ---- the class's diffuse light after scattering (SubsurfaceScatter.hlsli), with the pixel's f_d back
    {
        Texture2D<float4> scatterDiffuse = ResourceDescriptorHeap[P[9].z];
        Texture2D<float> sceneDepth = ResourceDescriptorHeap[P[0].y];
        // A first-person view model under viewmodel.fov_override_degrees is drawn with clip.xy x g_viewModelScale
        // (ViewModel.hlsli): its surface lies on the pixel's ray with the image-plane part divided by that scale, so the
        // scatter pass takes that ray - the radius in pixels is the true geometry's, the scale times the remapped ray's.
        float3 Ds = D, Dxs = Dx, Dys = Dy;
        if (P[8].x != UNX_NONE)
        {
            Texture2D<uint> visIds = ResourceDescriptorHeap[P[8].x];
            const uint visId = visIds[pixel];
            if (visId != VIS_NONE && (loadInstance(loadVisibleCluster(P[8].y, visVisibleCluster(visId)).instance).flags & INSTANCE_VIEW_MODEL) != 0)
            {
                const float k = 1 / g_viewModelScale;
                Ds = (D + g_view[2].xyz) * k - g_view[2].xyz;
                Dxs = Dx * k;
                Dys = Dy * k;
            }
        }
        // (an eye's iris keeps its own light: the mean free path x (1 - mask))
        radiance += diffuse * (sssScatter(scatterDiffuse, sceneDepth, pixel, Ds, Dxs, Dys, linearZ, n, s.baseColor * (1 - s.metallic), m.hairAbsorption * (1 - eye.mask),
                                          P[11].y, asfloat(P[11].z)) / g_exposure);
    }
#endif
#if SSS_SPLIT && SHADE_PART == 2
    // the specular light and the emission back to the direct radiance texture, this part's diffuse light per unit f_d onto
    // part 1's with the pixel's view depth (the scatter pass's sample test and plane distance): no output here
    {
        RWTexture2D<float4> direct = ResourceDescriptorHeap[P[10].x];
        RWTexture2D<float4> scatterDiffuse = ResourceDescriptorHeap[P[9].z];
        direct[pixel] = float4(radiance, 1);
        scatterDiffuse[pixel] = float4(min(scatterDiffuse[pixel].rgb + scatterE * g_exposure, 60000.0), linearZ);
    }
#elif SHADE_PART >= 2
    // ---- air between the camera and the surface (S's air volume: atmosphere, shadowed air, local lights' air)
    radiance = radiance * airTransmittance + airInscatter;

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].w];
    // P[5].zw: the view's particle layer and edge blocks (UNX_NONE: none).
    shExposureHistogram(P[4].w, radiance, pixel, asfloat(P[6].z));  // P[4].w histogram, P[6].z centre sigma (main view)
    const float3 withParticles = shParticles(radiance * g_exposure, pixel, P[5].z, P[5].w);
    color[pixel] = (P[4].z & 4096) ? float4(withParticles / g_exposure, 1) : shEncodeExposed(withParticles);
#endif
    ShadedPixel o;
    o.radiance = radiance;
#if SSS_SPLIT && SHADE_PART == 1
    o.scatter = scatterE;
#endif
    return o;
}
