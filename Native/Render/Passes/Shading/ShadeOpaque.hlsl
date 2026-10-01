// unx-kernel: cs_6_6 main
// unx-variants: FALLBACK=0,1 AREA=0,1 PLANAR=0,1 LAYERED=0,1,2
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
// Classes without their own model yet (Subsurface, Water: INTERFACES 8.1 defines them before P3/P4) use this kernel.
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
// LAYERED: 1 = A9 clearcoat (shade class Layered), 2 = A9 sheen (shade class Sheen; MATERIAL_LAYERS 1.4).
// P[1] = { tile lists (raw), list offset (entries), shade class (bit 31 set: a mask of classes, 0xFFFFFFFF every non-sky
//        class - the fallback kernel runs once per LAYERED variant over its classes), emissive or
//        UNX_NONE }
// P[2] = { shadow visibility, screen probes, reflection, GI cache (planar views) } (UNX_NONE = absent)
// P[3] = { atmosphere transmittance, multi-scatter, S's shadow overflow tile heads (main kernel; UNX_NONE = absent), this
//        view's air volume } (this kernel reads no sky view)
// P[4] = { B2 stable area lights' mask (raw, 1 bit per scene light; UNX_NONE = none), texture table, experiment mask (0;
//        shading.toml), L2 tile lights' records (raw, TileLights.hlsli; UNX_NONE = off) }
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
// P[9].y A9 anisotropy word (Resolve.hlsl; UNX_NONE = no anisotropic material): read by the LAYERED variants, whose
//        anisotropic pixels shade the base specular with the anisotropic lobe (AnisoShading.hlsli)
// P[8] = { W's sun-space water map (v1.77): waterSunDepth, waterSunNormal, waterSunMedium, waterSunConstants (UNX_NONE:
//        no water) } - a surface under water from the sun takes the refracted sun direction and the water's transmittance
//        (Passes/Water/WaterLight.hlsli waterSunLight); P[9].x its caustics (waterSunCaustics, UNX_NONE: none)
#ifndef AREA_LOBES
#define AREA_LOBES 0  // AreaLobes.hlsl compiles this file with 1: the area-light lobe terms alone (see there)
#endif
#ifndef SHADE_PART
#define SHADE_PART 1  // ShadeIndirect.hlsl compiles this file with 2 (see the header)
#endif
#ifndef OUTPUT
#define OUTPUT 0      // (part 1 writes no output)
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
#if AREA_LOBES
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

struct ShadedPixel
{
    float3 radiance;  // linear, before exposure
};

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, uint2 gbPacked, float depthValue,
                         uint overflowHead);


[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
    const uint tile = tiles.Load(4 * (P[1].y + gid.x));
    const uint2 tileCoord = uint2(tile & 0xFFFFu, tile >> 16);
    const uint2 pixel = tileCoord * M_TILE + tid;
    // The group's independent reads go out together before any of them is waited on: the records of the tile's 2 x 2 corner
    // screen probes (R's tile cache, split form: the probe counts come from the frame constants, so no header read comes
    // first), S's overflow tile head, and the pixel's material word, G-buffer and depth. The records then go to groupshared
    // [measured, city 4K: shading 1.257 -> 1.191 ms against loading the tile, then the pixel]. The probe condition is
    // uniform (root constants), so the whole group reaches the barrier.
    const uint lane = tid.y * M_TILE + tid.x;
#if SHADE_PART == 2 && !PLANAR
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
#if !FALLBACK
    if (P[3].z != UNX_NONE)
    {
        Texture2D<uint> heads = ResourceDescriptorHeap[P[3].z];
        overflowHead = heads[tileCoord];
        if (overflowHead == 0xFFFFFFFFu) return;  // over the list's capacity: the fallback kernel shades this tile
    }
#endif
#if SHADE_PART == 2 && !PLANAR
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
#if AREA_LOBES
    return;  // (the lobe texture is written; edges keep the full kernel's radiance)
#endif
#if SHADE_PART == 1
    // part 1's result: the direct radiance of this class's pixels (other classes' pixels are left to their own runs)
    if (active)
    {
        RWTexture2D<float4> direct = ResourceDescriptorHeap[P[10].x];
        direct[pixel] = float4(sp.radiance, 1);
    }
    return;
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
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;
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

#if SHADE_PART == 2
    float3 radiance;
    {
        RWTexture2D<float4> direct = ResourceDescriptorHeap[P[10].x];  // part 1's direct radiance (ShadeOpaque.hlsl)
        radiance = direct[pixel].rgb;
    }
#elif AREA_LOBES
    float3 radiance = 0;
    if (false)
#else
    float3 radiance = m.emissive;
    if (P[1].w != UNX_NONE && mLoadTextureSet(P[4].y, materialIndex).emissive != UNX_NONE)
#endif
#if SHADE_PART == 1
    {
        Texture2D<float4> emissive = ResourceDescriptorHeap[P[1].w];
        radiance = emissive[pixel].rgb;  // material emissive x texture, resolved at the footprint
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
#if SHADE_PART == 1
    float sunVisibility = 1;
    if (P[2].x != UNX_NONE && (P[4].z & 2048) == 0)
    {
        Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
        sunVisibility = shadowSlot(shadow[pixel], 0);
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
                const float e = modelDirectionalAlbedo(NoV, s.roughness);
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap;
                // lobe 0: the base's specular; A9 lobe 1: the coat's (F = 1 through the same disk rules, then the exact
                // dielectric Fresnel and A2's scale at the disk centre, or for a lobe narrower than the disk its albedo
                // E_ms(n.v) over the single-scattering E(n.v)): one inlined disk integral for both
                uint lobes = 1;
#if LAYERED == 1
                if (cover > 0) lobes = 2;
#endif
                [loop] for (uint lobe = 0; lobe < lobes; ++lobe)
                {
                    const bool coatLobe = lobe == 1;
#if LAYERED == 1
                    const float3 lf0 = coatLobe ? 1.0.xxx : f0, lcomp = coatLobe ? 1.0.xxx : compensation;
                    const float lr = coatLobe ? coat.roughness : s.roughness;
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
                    sun += spec;
                }
            }
            if (foliage) sun += back * below * cap;
        }
        else if (foliage) sun = back * above * cap;  // viewer behind the shading normal: only light crossing the leaf
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
        radiance += sun * sunVisibility;
    }

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
    if (froxels.lights != UNX_NONE && (experiment & 32) == 0)  // this view's lists (v1.22)
    {
        uint shadowPacked = 0xFFFFFFFFu;  // all slots lit when S publishes no visibility
        if (P[2].x != UNX_NONE && (experiment & 2048) == 0)
        {
            Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
            shadowPacked = shadow[pixel];
        }
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
        const float3 compensation = 1 + f0 * (1 / e - 1);
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        const uint indexBase = froxelIndexBase(froxels);
        uint shadowOrdinal = 0, overflowRecord = 0xFFFFFFFFu;
        uint2 overflowPacked = uint2(0xFFFFFFFFu, 0);
#if !FALLBACK
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
        float3x3 specular = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), s.roughness), frame);
        float3 specularAlbedo = shSpecularAlbedo(f0, max(NoV, 1e-4), s.roughness);
#if LAYERED
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
#if !FALLBACK  // (fallback tiles keep the full read path: the DXIL limit)
        if (P[10].w != UNX_NONE)
        {
            clsTile = vsmClsTile(P[10].w, (pixel.y / M_TILE) * ((g_viewWidth + M_TILE - 1) / M_TILE) + pixel.x / M_TILE);
            clsSlice = froxelSlice(froxelGrid(froxels.lights), linearZ);
        }
#endif
        // L2 (14.1/14.2): this tile's record: FAR lights (a clear bit of the pixel's slice mask) skip their diffuse term
        // here and come back as the tile corners' vector irradiance below; Foliage keeps every light per pixel.
#if !FALLBACK  // (fallback tiles - S's overflow - keep every light per pixel: the record is an optimisation, not a value)
        if (P[4].w != UNX_NONE && !foliage)
        {
            tileRec = tileLightsRecord(P[4].w, (pixel.y / M_TILE) * ((g_viewWidth + M_TILE - 1) / M_TILE) + pixel.x / M_TILE);
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
            const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
            const bool isFar = tileFar && i < 64 && (((i < 32 ? nearMask.x : nearMask.y) >> (i & 31)) & 1u) == 0;
            const float3 frontL = isFar ? 0 : front;  // (L2: the FAR light's diffuse is in the tile term)
            const GpuLight light = loadLight(lightIndex);
            // The light's shadow ordinal is counted here; its visibility (a slot, or the overflow records' dependent
            // loads) is read only for a light that adds something at this pixel (a window, a spot factor and a lobe on
            // the side it lights): the others added exactly 0.
            const bool casts = lightCastsShadow(light);
            if (casts) ++shadowOrdinal;
            // diagnostic 16384 (R's request 2026-10-01, the 128-slot limit's share): a shadow-casting light that has no VSM slot
            // this frame (froxel entry bit 15 clear: S lit it unshadowed) adds nothing; L3 stage 5 removes the limit
            if ((experiment & 16384) != 0 && casts && (froxelEntryAt(froxels, indexBase, range.x + i) & 0x8000u) == 0) continue;
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
                if (all(E == 0) || !((NoV > 0 && cosL > 0) || (foliage && NoV * cosL < 0))) continue;
            }
            float visibility = 1;
#if FALLBACK
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
                const float3 Lw = light.color * (light.intensity * window * visibility);
#if AREA_LOBES
                // A9 the lobes no LTC represents, over the light (AreaQuadrature.hlsli): the anisotropic base (MATERIAL_LAYERS
                // 1.5; under a coat scaled like the base) and the sheen (1.4) - on the viewer's side
                if (NoV > 0)
                {
#if LAYERED == 1
                    if (aniso.on && !shLightSpecularInResult(lightIndex))
                        radiance += ((cover > 0) ? keep : 1.0) * Lw * shAreaAniso(light, p, aniso.t, aniso.b, n, v, aniso.alpha, f0, 1 + f0 * (1 / (aniso.ab.x + aniso.ab.y) - 1));
#endif
#if LAYERED == 2
                    radiance += Lw * sheen.color * shAreaSheen(light, p, frame, v, sheen.roughness);
#endif
                }
                continue;
#endif
                // Integrals in order: front diffuse, specular, back (Foliage) -- one inlined evaluator (the diffuse frames
                // are rotations: closed forms on circular cones).
                uint first = NoV > 0 ? 0 : 2, last = foliage ? 3 : 2;
                const bool specularInReflections = shLightSpecularInResult(lightIndex);  // P[4].x: B2 mask (planar views: none)
                float scaleBase = 1;
#if LAYERED == 2
                scaleBase = keepS;  // (the sheen lobe over area lights: the lobe texture, AreaLobes.hlsl)
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
                    if ((j == 1 || j >= 3) && specularInReflections) continue;
#if LAYERED
                    if (j == 1 && aniso.on) continue;  // (the anisotropic lobe over the light: the lobe texture, AreaLobes.hlsl)
#endif
                    if (j == 2 && !foliage) continue;
#if LAYERED == 1
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (j == 3 ? coatSpecular : (j == 4 ? coatBase : (NoV > 0 ? frameBack : frame))));
#else
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (NoV > 0 ? frameBack : frame));
#endif
                    const float I = shAreaIntegral(light, p, T, j == 0 || j == 2);
#if LAYERED == 1
                    if (j == 0) coatId = I;
                    if (j >= 3)
                    {
                        coatAdd += (j == 3 ? coatAlbedo : tvtl * coatBaseAlbedo) * I;
                        continue;
                    }
#endif
                    radiance += scaleBase * Lw * (j == 0 ? frontL * (SH_PI * I) : (j == 1 ? specularAlbedo * I : back * (SH_PI * I)));
                }
#if LAYERED == 1
                if (last == 5)
                {
                    const float muIn = modelCoatRefractedCos(max(dot(n, normalize(p)), 1e-4), coat.eta);
                    coatAdd += tvtl * (diffuse + modelCoatReturned(s, coat, muIn) / SH_PI) * (SH_PI * coatId);
                    radiance += cover * Lw * coatAdd;
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
            if (NoV > 0 && cosL > 0) f = frontL + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
#if LAYERED
            if (aniso.on && NoV > 0 && cosL > 0) f = frontL + shAnisoSpecular(aniso, f0, n, v, l);
#endif
#if LAYERED == 1
            if (cover > 0) f = keep * f + cover * (modelCoatLobe(coat, n, v, l) + modelCoatUnder(s, coat, n, v, l));
#endif
#if LAYERED == 2
            if (NoV > 0 && cosL > 0) f = keepS * f + sheen.color * modelSheenLobe(sheen.roughness, n, v, l);
#endif
            radiance += f * E * (abs(cosL) * visibility);
        }
    }

    // L2 (14.2): the FAR lights' diffuse, once: the tile corners' vector irradiance (every FAR light above the tile's
    // normal cone with margin, so n . E is their exact sum up to the 1e-3 interpolation rule), bilinear in the tile.
#if !AREA_LOBES && !FALLBACK
    if (tileFar && NoV > 0) radiance += front * max(0.0, dot(n, tileLightsIrradiance(tileRec, pixel & (M_TILE - 1))));
#endif

    // 14.1b (L2b): the converted emissive surfaces as area lights - their diffuse irradiance on the viewer's side of n
    // (EmissiveDirect.hlsl: quadtree nodes as horizon-clipped Lambert polygons; the specular side is the reflection
    // path's, which sees the emissive geometry: B2). Node shadows: 14.3 (L3). Foliage's back side: not yet.
#if !AREA_LOBES
    if (P[10].z != UNX_NONE && NoV > 0)
    {
        Texture2D<float4> emissiveE = ResourceDescriptorHeap[P[10].z];
        radiance += front * (emissiveE[pixel].rgb / g_exposure);
    }
#endif
#endif  // SHADE_PART == 1 (the sun, the local lights, the tile term, the emissive irradiance)

#if AREA_LOBES
    {
        RWTexture2D<float4> lobes = ResourceDescriptorHeap[P[9].z];
        lobes[pixel] = float4(radiance * g_exposure, 0);  // (exposed: f16 range)
    }
#elif SHADE_PART == 2
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
        const bool wantRadiance = specular && refl.a <= 0;
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
        if (specular && refl.a > 0) incident = refl.rgb;
    }
#else
    if (P[2].w != UNX_NONE)
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
        if (foliage) irradianceBack = giCacheIrradiance(gi, worldPos, -nv);
        if (NoV > 0) incident = giCacheRadiance(gi, worldPos, n, r, halfAngle);  // looked up in this surface's normal class
#if LAYERED == 1
        if (NoV > 0 && cover > 0) coatIncident = giCacheRadiance(gi, worldPos, n, r, reflectionLobeHalfAngle(coat.roughness, NoV));
#endif
    }
#endif
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
    if (NoV > 0) radiance += keepS * (front * irradiance + incident * baseAlbedo) + sheen.color * (modelSheenAlbedo(NoV, sheen.roughness) / SH_PI) * irradiance;
    else
#endif
    if (NoV > 0) radiance += front * irradiance + incident * baseAlbedo;
    radiance += back * irradianceBack;

#if LAYERED
    // A9: the area-light lobes no LTC represents (AreaLobes.hlsl, dispatched before this kernel on the same tiles;
    // area-lit scenes only: P[9].z)
    if (P[9].z != UNX_NONE)
    {
        RWTexture2D<float4> lobes = ResourceDescriptorHeap[P[9].z];
        radiance += lobes[pixel].rgb / g_exposure;
    }
#endif
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
    return o;
}
