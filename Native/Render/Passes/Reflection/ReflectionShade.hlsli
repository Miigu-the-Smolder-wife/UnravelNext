// Shading of one reflection hit (R-internal), without tracing, so compute passes can run it (r.refl.shade, ARCHITECTURE
// 2.6 revision 1: traversal in DispatchRays, hit shading in compute) as well as the inline path (ReflectionHit.hlsli).
// Needs GiSky.hlsli's root constants (P[1] to P[3]), the GI cache (RW), P[5].x = frame (low 24 bits) |
// reflection.experiment_disable << 24, P[5].z = a raw buffer holding this frame's ShadowSrvs (UNX_NONE: no VSM) and
// P[5].w = RayScene's exact set hit counts UAV (UNX_NONE: none). Experiment bits (cost attribution only, 0 in the shipped
// configuration): 1 = sun shadow rays without any-hit, 2 = no sun visibility (the sun taken as visible), 4 = sun
// visibility by shadow rays only, 8 = hit materials without textures, 16 = no cache lookup at hits, 32 = traversal only
// (a hit returns 1).
//
// The hit's footprint is the ray cone's width there (Akenine-Moller et al., ray cones): 'coneWidth' at the ray origin (the
// primary pixel's width at the reflector, pixel spread x eye distance) plus t x 'coneSpread' (full angle: the pixel's spread
// plus the lobe's 2 tan(half-angle); reflector curvature is not included, so on curved mirrors it is a lower bound). It
// picks the texture level, the cache level and the VSM level at the hit, and filters the sun's specular highlight.
// A reflection hit consumes the cache like a GI hit: its cell (at the footprint's level) is found or created and
// requested for update next frame (hit tier), so cells only reflections see are kept converged.
#ifndef UNX_REFLECTION_SHADE_HLSLI
#define UNX_REFLECTION_SHADE_HLSLI
#include "RayTracing/RayScene.hlsli"
#ifndef RT_HIT_EYE
#define RT_HIT_EYE 1  // a reflected eye shows its iris through the cornea (HitShading.hlsli rtHitMaterialSeen)
#endif
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiAccPool.hlsli"
#include "Passes/SurfaceCache/SurfaceCache.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

// Motion of a hit since the previous tick, in units of the ray's footprint there: |x - x_prev| / footprint, x_prev the
// same barycentric point of the triangle's previous-tick vertices (deformed instances: the pool's per-vertex world -
// prevWorld that RayTracing/Deform.hlsl writes with deformVertex, the function R's refit uses; rigid instances: the
// previous transform of the object-space point). Static instances: 0.
// The time integration keeps n x motion <= reflection.temporal_lobe_shift: the reflected content may move at most that
// share of the footprint over the window.
// inst, mesh, ri, tri: the hit's records (rtSurfaceParts).
float reflHitMotion(RtSceneSrvs scene, RtHit hit, GpuInstance inst, GpuMesh mesh, RtInstance ri, RtTriangle tri, float footprint)
{
    const bool deformed = (ri.flags & RT_INSTANCE_DEFORMED) != 0;
    if (!deformed && all(inst.objectToWorld[0] == inst.prevObjectToWorld[0]) && all(inst.objectToWorld[1] == inst.prevObjectToWorld[1]) &&
        all(inst.objectToWorld[2] == inst.prevObjectToWorld[2]))
        return 0;
    const float3 w = rtBary(hit.barycentrics);
    float3 delta;
    if (deformed)
    {
        StructuredBuffer<RtDeformedVertex> d = ResourceDescriptorHeap[scene.deformed];
        const uint2 a = d[ri.vertexBase + tri.poolIndex.x].motion, b = d[ri.vertexBase + tri.poolIndex.y].motion, c = d[ri.vertexBase + tri.poolIndex.z].motion;
        delta = float3(f16tof32(a.x), f16tof32(a.x >> 16), f16tof32(a.y)) * w.x + float3(f16tof32(b.x), f16tof32(b.x >> 16), f16tof32(b.y)) * w.y +
                float3(f16tof32(c.x), f16tof32(c.x >> 16), f16tof32(c.y)) * w.z;
    }
    else
    {
        const float3 p = loadVertex(mesh, tri.meshVertex.x).position * w.x + loadVertex(mesh, tri.meshVertex.y).position * w.y +
                         loadVertex(mesh, tri.meshVertex.z).position * w.z;
        delta = transformPoint(inst.objectToWorld, p) - transformPoint(inst.prevObjectToWorld, p);
    }
    return length(delta) / max(footprint, 1e-6);
}

// radiance = the hit's radiance toward the ray origin when the sun term is resolved (no sun, or S's VSM holds the hit);
// otherwise the sun term at full visibility is in sunTerm and needs one shadow ray from shadowOrigin toward a point of
// the solar disk: the value is radiance + sunTerm x visibility.
struct ReflHitShade
{
    float3 radiance, sunTerm, shadowOrigin;
    bool needsShadowRay;
    float motion;  // the hit's displacement since the previous tick over the footprint (reflHitMotion)
    // REFL_DEFER_PENUMBRA (r.refl.shade): S's VSM holds the hit but its region needs the penumbra filter; sunTerm is the
    // sun term at full visibility times the transmittance, the value radiance + sunTerm x shadowSunPenumbraDeferred(
    // shadowOrigin = the hit point, penumbraNormal, penumbraLevel, penumbraReach) (ReflectionPenumbra.hlsl).
    bool needsPenumbra;
    float3 penumbraNormal;
    uint penumbraLevel;
    float penumbraReach;
    // Reconstruction layers (HitShading.hlsli rtHitRadianceSplit): the part of 'radiance' that is the hit's stochastic
    // light, its demodulation albedo and the hit's shading normal; surface = false: an emitter, or no value (all in base).
    float3 stochastic, albedo, hitNormal;
    bool surface;
    bool noData;  // the cache lookup at the hit found nothing (its indirect light is 0)
};

// This frame's ShadowSrvs (P[5].z raw buffer; P[5].z = UNX_NONE: none).
ShadowSrvs reflShadowSrvs()
{
    ByteAddressBuffer b = ResourceDescriptorHeap[P[5].z];
    const uint4 a = b.Load4(0), c = b.Load4(16);
    ShadowSrvs vsm;
    vsm.pageTable = a.x; vsm.pool = a.y; vsm.blocks = a.z; vsm.searchBound = a.w;
    vsm.constants = c.x; vsm.lights = c.y; vsm.pad0 = c.z; vsm.layers = c.w;
    return vsm;
}

// Flags of the hit shading (the rays header's word 7, ReflectionSystem; the passes that have the rays buffer set
// g_reflHitFlags from it before they shade - other users of reflShadeHit, refraction and test views, keep 0):
//   REFL_HIT_CONE_LOBES  reflection.hit_cone_lobes: the hit's specular lobes toward its lights are widened by the ray
//                        cone, alpha' = sqrt(alpha^2 + (cone spread / 2)^2) - the base lobe for the local-light sample
//                        and the coat lobe for the local lights and the sun (HitLayers.hlsli g_rtHitCone). A reflection
//                        value is the mean over its ray cone of what leaves the hit toward the reflector; for a
//                        specular lobe at the hit that mean is the lobe widened by the cone. GI hits have done both
//                        since 2026-09-27 (GiTrace: "a glossy hit's point-light highlight came in as rare huge
//                        samples that stayed ... as bright dots"), and the sun's base highlight is filtered by the
//                        cone here too (rtHitRadianceSplit's pixelAngle); the reflection hits' local-light lobe and
//                        coat lobe were evaluated at the single ray direction - a G ray (cone of several degrees)
//                        meeting a glazed tile near a lamp's mirror direction returned the unfiltered highlight.
//   REFL_HIT_ORIENTED    reflection.hit_oriented_lights: the hit's one local-light sample is chosen with the hit's
//                        orientation in the weights (HitLocalLights.hlsli rtLocalLightChooseOriented): lights below
//                        the hit's horizon are not drawn. Same estimator count (one sample, one shadow ray), unbiased.
//   REFL_HIT_STRICT_READ the hit's cache read follows gi.bounce_visibility (only corners whose anchor sees the hit). Set
//                        with the reconstruction layers and their filter only: a hit that then finds no corner has no
//                        data, and it is the layers' filter that gives such a pixel its neighbours' value - without
//                        it the hit's indirect light would be 0 (black reflection samples after a cut).
#define REFL_HIT_CONE_LOBES 1u
#define REFL_HIT_ORIENTED 2u
#define REFL_HIT_STRICT_READ 4u
#define REFL_HIT_SC_VIEW 8u  // diagnostics (reflection.lumen_surface_cache_view): the hit's value is its surface cache state
static uint g_reflHitFlags = 0;
// The GI hit accumulator pool (GiAccPool.hlsli; RENDERER_REDESIGN_V2 12.1 / 12.8), UNX_NONE: none - set with the flags
// from the rays header (word 8). A hit's diffuse direct light (sun and the local-light sample on the hit's diffuse and
// coat-transmitted terms) is the light of one point; where a lamp's 1 / d^2 hotspot lies inside the ray's footprint, the
// point value is heavy-tailed (measured: half of a glossy wall's reflected energy in 1.5-5 % of its samples). With the
// pool, the hit takes the mean of those terms over the surface cell its footprint covers (the GI rays' hits of the last
// frames) with its own albedo and layers: radiance += weight x (kA mA + kB mB + kC mC - the point's diffuse direct term).
// Read only: the pool's ratio estimator keeps energy for the population that recorded (the GI rays). A ray whose
// footprint is far below the cell read (a mirror ray near its surface: cell > 4 x footprint) keeps its point value - the
// cell mean would blur the direct light's edges in the mirror image to the cell's size. Foliage keeps the point value
// (its transmission has no accumulator term). The overflow libraries (ReflectionTraceInline) read it too: their size came
// down by holding the sun's quadrature and the screen-probe lookup once each (ShadingCommon.hlsli, ReflectionRay.hlsli).
static uint g_reflAccPool = UNX_NONE;
// The surface cache (Passes/SurfaceCache/SurfaceCache.hlsli; the rays header's word 9), UNX_NONE: not used. With it a hit
// marks its cell (the cell stays alive and lit every frame from then on) and, where the cell has lighting, takes its
// local-light and bounce irradiance from the cell: no one-light sample, no cache lookup noise - the hit's light is what
// the cell accumulated, as a Lumen reflection ray reads the surface cache at its hit. The hit keeps its own material
// (textures at the ray's footprint), emission and sun term. Until the cell is lit (one frame after its first mark) the
// hit is shaded as before.
static uint g_reflSurfaceCache = UNX_NONE;
// surface_cache.mesh_cards: the card frame's SRV (CardLayout.hlsli); the hit reads its irradiance from the mesh cards of
// its instance (CardLighting.hlsli clReadCards) in place of the hashed cells.
static uint g_reflCardFrame = UNX_NONE;

// Local lights (HitLocalLights.hlsli): one next-event sample drawn with localSeed; its visibility is localVisible (the
// compute path: ReflectionLocalShadow traced it before, same seed and hit point) or, with REFL_LOCAL_TRACE (the ray
// generation paths), traced here. Lights that cast no shadow: visible.
// localChoice: the light ReflectionLocalShadow chose for this hit (rtPackLocalChoice; x = REFL_NO_CHOICE: choose here).
#define REFL_NO_CHOICE 0x7FFFFFFFu
ReflHitShade reflShadeHit(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RtHit hit, float3 origin, float3 direction, float coneWidth,
                          float coneSpread, uint localSeed, bool localVisible, uint2 localChoice = uint2(REFL_NO_CHOICE, 0))
{
    ReflHitShade o;
    o.radiance = o.sunTerm = o.shadowOrigin = 0;
    o.needsShadowRay = false;
    o.motion = 0;
    o.needsPenumbra = false;
    o.penumbraNormal = 0;
    o.penumbraLevel = 0;
    o.penumbraReach = 0;
    o.stochastic = 0;
    o.albedo = 1;
    o.hitNormal = -direction;
    o.surface = false;
    o.noData = false;
    const uint experiment = P[5].x >> 24;
    if (hit.instance == RT_INSTANCE_EMITTER)
    {
        // An analytic area light (raytracing.emitters, design 12.4 structure 2): its radiance, seen from the reflector.
        o.radiance = rtEmitterCounts(scene.pad, hit.primitive) ? rtEmitterRadiance(hit.primitive, origin) : float3(0, 0, 0);
        return o;
    }
    if (experiment & 32)
    {
        o.radiance = 1;  // attribution: traversal only
        return o;
    }
    // Reflection exact set: count this frame's M/G hits on skinned instances (RayScene selects the most-hit ones).
    if (P[5].w != UNX_NONE)
    {
        RtGeometry unused;
        const RtInstance ri = rtResolve(scene, hit, unused);
        const uint deformedIndex = ri.flags >> 8;
        if ((ri.flags & RT_INSTANCE_DEFORMED) != 0 && deformedIndex != 0xFFFFFFu)
        {
            RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[P[5].w];
            InterlockedAdd(counts[deformedIndex], 1u);
        }
    }
    GpuInstance hitInst;
    GpuMesh hitMesh;
    RtInstance hitRi;
    RtTriangle hitTri;
    const RtSurface s = rtSurfaceParts(scene, hit, origin, direction, hitInst, hitMesh, hitRi, hitTri);
    const float footprint = coneWidth + hit.t * coneSpread;
    o.motion = reflHitMotion(scene, hit, hitInst, hitMesh, hitRi, hitTri, footprint);
    GpuMaterial m = loadMaterial(s.material);
    if ((experiment & 8) == 0)
    {
        m = rtHitMaterialSeen(m, s, direction, footprint, dot(s.normal, direction));
        rtHitDecals(scene, s, footprint, m);  // the direct view's decals (A7)
    }
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return o;  // inside closed geometry
    const uint footprintLevel = giLevelForSize(h, footprint);
    if (g_reflHitFlags & REFL_HIT_CONE_LOBES) g_rtHitCone = 0.5 * coneSpread;
    RtHitLighting L;
    L.irradiance = L.specularRadiance = L.local = 0;
    float3 localE = 0;  // the local-light sample's irradiance on the hit (n.l x weight; 0 when shadowed) and its cosine
    float localMu = 0;
    {
        RtLocalChoice choice;
#if REFL_CHOICE_GIVEN
        choice = rtUnpackLocalChoice(localChoice);  // (r.refl.shade: ReflectionLocalShadow chose for every hit)
#else
        // (one function for both settings: without REFL_HIT_ORIENTED its weights and arithmetic are rtLocalLightChoose's)
        if (localChoice.x != REFL_NO_CHOICE) choice = rtUnpackLocalChoice(localChoice);
        else choice = rtLocalLightChooseOriented(scene, s.position, s.normal, (g_reflHitFlags & REFL_HIT_ORIENTED) == 0 || rtHitTransmits(m), giUnit(localSeed));
#endif
        const RtLocalSample ls = rtLocalLightFinish(scene, choice, s.position, giUnit(localSeed + 1), giUnit(localSeed + 2), footprint);
        if (ls.valid)
        {
            GpuMaterial mc = m;  // the base lobe widened by the ray cone (REFL_HIT_CONE_LOBES; the coat lobe: g_rtHitCone)
            if (g_reflHitFlags & REFL_HIT_CONE_LOBES)
            {
                const float alpha = modelAlpha(m.roughness), cone = 0.5 * coneSpread;
                mc.roughness = sqrt(sqrt(alpha * alpha + cone * cone));
            }
            const float3 f = rtLocalLightBrdfCos(mc, s.normal, -direction, ls.wi, false);
            bool visible = !ls.castShadow || localVisible;
#if REFL_LOCAL_TRACE
            if (any(f > 0))  // (f = 0: the visibility multiplies nothing, no ray)
                visible = !ls.castShadow ||
                          rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, 1e-3 + 2e-4 * distance(s.position, g_cameraPosition)), RT_MASK_HIT_SHADOW);
#endif
            if (visible)
            {
                L.local = f * ls.weight;
                localMu = max(dot(s.normal, ls.wi), 0.0);
                localE = localMu * ls.weight;
            }
        }
    }
    if ((experiment & 16) == 0)
    {
        if ((experiment & 256) == 0)  // 256: reflection hits read the cache only (the estimator's bias apart from cache feedback)
        {
            bool created;
            const uint e = giFindOrCreate(cache, h, giSurfaceKey(h, s.position, s.normal, footprintLevel), giAnchorAtHit(h, s.position, direction, hit.t), s.normal, created);
            if (e != GI_ENTRY_PENDING)
            {
                if (cache.Load(h.offHitStamp + e * 4) != h.frame)  // (stamped this frame: touched and requested already, giKeepRead)
                {
                    giTouch(cache, h, e);
                    giRequestHit(cache, h, e);
                }
            }
        }
        g_giKeepReads = (experiment & 256) == 0;
        float3 sumE, sumL;
        float weight;
        // gi.bounce_visibility (GI_P1_FLAGS bit 6; GiCache.hlsli g_giStrictVisibility, as GiTrace's fallback read): only
        // the corners whose anchor sees the hit - after a cut every level is young, and the coarse cells then carried light
        // from behind walls into the hits (R: lounge, daylight from outside at 1.5-1.8 x [measured]). A hit that finds
        // no such corner has no data (the layers' filter gives it its neighbours' value: REFL_HIT_STRICT_READ).
        g_giStrictVisibility = (g_reflHitFlags & REFL_HIT_STRICT_READ) != 0 && (cache.Load(GI_P1_FLAGS) & 64u) != 0;
        giCacheLevels(cache, h, s.position, s.normal, reflect(direction, s.normal), true, footprintLevel, sumE, sumL, weight);
        g_giStrictVisibility = false;
        L.irradiance = weight > 0 ? sumE / weight : 0;
        L.specularRadiance = weight > 0 ? sumL / weight : 0;
        o.noData = weight <= 0;
        // Diagnostics: lookups and misses (no updated cell at any level searched), one atomic per wave.
        const uint lookups = WaveActiveCountBits(true), misses = WaveActiveCountBits(weight <= 0);
        if (WaveIsFirstLane())
        {
            cache.InterlockedAdd(GI_H_STAT_HIT_LOOKUPS, lookups);
            if (misses) cache.InterlockedAdd(GI_H_STAT_HIT_MISSES, misses);
        }
    }
    bool fromSurfaceCache = false;
    bool fromCards = false;  // (the cards' direct light holds the sun: the hit adds none)
    if (g_reflCardFrame != UNX_NONE && materialClass(m) != MATERIAL_FOLIAGE)
    {
        const float3 face = dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal;
        const ClSample cards = clReadCards(mcFrame(g_reflCardFrame), s.sceneInstance, s.position, face, CL_READ_IRRADIANCE);
        if (g_reflHitFlags & REFL_HIT_SC_VIEW)
        {
            // r: 16 = the hit read no card, g: 16 = it read one; components 1, 2: the cards' direct and indirect
            // irradiance as E / pi
            const uint component = (g_reflHitFlags >> 8) & 7u;
            o.radiance = float3(cards.valid ? 0.0 : 16.0, cards.valid ? 16.0 : 0.0, 0);
            if (component == 1) o.radiance = cards.direct / MODEL_PI;
            if (component == 2) o.radiance = cards.indirect / MODEL_PI;
            return o;
        }
        if (cards.valid)
        {
            L.irradiance = cards.direct + cards.indirect;
            L.specularRadiance = L.irradiance / MODEL_PI;  // (the lobe at the hit sees the cards' light as uniform)
            L.local = 0;
            localE = 0;
            localMu = 0;
            o.noData = false;
            fromSurfaceCache = true;
            fromCards = true;
        }
    }
    else
    if (g_reflSurfaceCache != UNX_NONE && materialClass(m) != MATERIAL_FOLIAGE)
    {
        RWByteAddressBuffer surfaceCache = ResourceDescriptorHeap[g_reflSurfaceCache];
        const ScLayout layout = scLayout(surfaceCache);
        const float3 face = dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal;
        const float3 bounceAlbedo = saturate(m.baseColor * (1 - m.metallic) + 0.45 * lerp(float3(0.04, 0.04, 0.04), m.baseColor, m.metallic));
        scMark(surfaceCache, layout, s.position, face, bounceAlbedo, m.emissive);
        const ScSample cell = scRead(surfaceCache, layout, s.position, face);
        if (g_reflHitFlags & REFL_HIT_SC_VIEW)
        {
            // r: 16 = the hit found no lit cell, g: 16 = it found one, b: lit cells / 65536 (the same everywhere)
            o.radiance = float3(cell.valid ? 0.0 : 16.0, cell.valid ? 16.0 : 0.0, surfaceCache.Load(8) / 65536.0);
            // reflection.lumen_surface_cache_view_component (bits 8-10 of the flags) 1..4: an irradiance at the hit as
            // E / pi, 0 where the hit has no lit cell (the same hits in every component, so their means compare): the
            // cell's direct light, the cell's indirect light, the world GI cache's irradiance there (what the hit
            // shading without the surface cache reads) and that shading's one-sample local-light irradiance; 5, 6: the
            // cell's stored emission and the hit material's emission (radiance).
            const uint component = (g_reflHitFlags >> 8) & 7u;
            if (component != 0)
            {
                const float3 e = component == 1 ? cell.direct : component == 2 ? cell.indirect : component == 3 ? L.irradiance : localE;
                o.radiance = cell.valid ? e / MODEL_PI : 0;
                // 5, 6: the emission the cell holds and the hit material's own (radiance)
                if (component == 5) o.radiance = cell.valid ? cell.emission : 0;
                if (component == 6) o.radiance = cell.valid ? m.emissive : 0;
                // 7 (with surface_cache.debug_count): r = the share of this frame's radiosity rays that met geometry and
                // read no light, g = those rays / 65536 (the same in every pixel)
                if (component == 7) o.radiance = float3(surfaceCache.Load(60) / max((float)surfaceCache.Load(56), 1.0), surfaceCache.Load(56) / 65536.0, 0);
            }
            return o;
        }
        if (cell.valid)
        {
            L.irradiance = cell.direct + cell.indirect;
            L.specularRadiance = L.irradiance / MODEL_PI;  // (the lobe at the hit sees the cell's light as uniform)
            L.local = 0;
            localE = 0;
            localMu = 0;
            o.noData = false;
            fromSurfaceCache = true;
        }
    }
    const float3 v = -direction;
    L.sunIlluminance = 0;
    L.sunVisibility = 0;
    // A sun term kept apart (a shadow ray or the deferred penumbra filter supplies its visibility): the value at
    // visibility 0 and the term at full visibility x splitScale (rtHitRadiance is linear in the visibility).
    bool split = false;
    float splitScale = 1;
    const float3 l = normalize(g_sunDirection);
    if (!fromCards && (dot(s.normal, l) > 0 || rtHitTransmits(m)))
    {
        L.sunIlluminance = giSunIlluminance(s.position);
        if (any(L.sunIlluminance > 0))
        {
            // The direct view's estimator from S's VSM where it holds the hit at the ray cone's footprint (SMRT disk
            // integral: shadows agree with the direct view and the planar camera); a shadow ray elsewhere.
            bool resident = false;
            if (P[5].z != UNX_NONE && (experiment & 6) == 0)
            {
#if REFL_DEFER_PENUMBRA
                const ShadowSunClassified sc = shadowSunClassifyAt(reflShadowSrvs(), s.position, s.geometricNormal, footprint);
                resident = sc.resident;
                L.sunVisibility = sc.visibility;
                if (resident && sc.penumbra)
                {
                    split = true;
                    splitScale = sc.transmittance;
                    o.shadowOrigin = s.position;
                    o.needsPenumbra = true;
                    o.penumbraNormal = s.geometricNormal;
                    o.penumbraLevel = sc.k;
                    o.penumbraReach = sc.reach;
                }
#else
                L.sunVisibility = shadowSunVisibilityAt(reflShadowSrvs(), s.position, s.geometricNormal, footprint, resident);
#endif
            }
            if (experiment & 2)
            {
                L.sunVisibility = 1;
                resident = true;
            }
            if (!resident)
            {
                // Sun term at full visibility apart; offset along the geometric normal on the sun's side (leaves
                // transmit: the ray leaves the lit side; the shading normal can point under the triangle).
                split = true;
                const float side = dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0;
                o.shadowOrigin = s.position + side * s.geometricNormal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
                o.needsShadowRay = true;
            }
        }
    }
    // One evaluation for every hit: the split ones take the value at visibility 0 and the sun's term at full visibility x
    // splitScale from it (rtHitRadianceParts). Two evaluations (visibility 0, then splitScale, subtracted) put a second
    // full hit shading into every wave holding one penumbra or shadow-ray hit (r.refl.shade 1.76 -> 2.05 ms at 1440p with
    // the penumbra deferral, 1db06e4 [measured]).
    if (split) L.sunVisibility = 0;
    float3 sunFull;
    RtHitSplit parts;
    o.radiance = rtHitRadianceSplit(m, s.normal, v, L, coneSpread, split, sunFull, parts);
    if (split) o.sunTerm = sunFull * splitScale;
    if (g_reflAccPool != UNX_NONE && materialClass(m) != MATERIAL_FOLIAGE && !fromSurfaceCache)
    {
        ByteAddressBuffer pool = ResourceDescriptorHeap[g_reflAccPool];
        GiAccMeans means;
        if (giAccPoolRead(pool, s.position, s.normal, footprint, means) && means.cellSize <= 4 * footprint)
        {
            // the point's terms with the sun at full visibility; its visibility multiplies the sun's share (linear)
            const float muS = max(dot(s.normal, l), 0.0);
            const RtHitDirect d = rtHitDirectTerms(m, s.normal, v, L.sunIlluminance * muS, muS, localE, localMu, false);
            const float3 fromCell = means.weight * (rtHitDirectFromMeans(d, means.A, means.B, means.C) - (d.own - d.ownSun));
            const float3 sunShare = means.weight * d.ownSun;
            o.radiance += fromCell;
            parts.stochastic += fromCell;
            if (split) o.sunTerm -= sunShare * splitScale;
            else o.radiance -= sunShare * L.sunVisibility;
        }
    }
    o.stochastic = parts.stochastic;
    o.albedo = parts.albedo;
    o.hitNormal = s.normal;
    o.surface = true;
    return o;
}

#endif
