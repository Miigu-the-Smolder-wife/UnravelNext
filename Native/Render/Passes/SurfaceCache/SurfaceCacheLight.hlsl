// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// The surface cache's capture and lighting (SurfaceCache.hlsli), three ray generation shaders, each one thread per item
// of its frame budget (the dispatch width, P[0].y):
//   SurfaceCacheSeedGen    capture. A path leaves the camera position in a direction uniform over the sphere and bounces
//                          P[3].w times (cosine distribution); every surface it meets marks its cell with its material.
//                          No view direction enters: the cells around the camera exist whether or not the view looks at
//                          them (the reference captures cards around the camera; this engine has no cards to capture).
//   SurfaceCacheCellsGen   direct lighting and the cell's indirect light. Cells not lit yet first, then a window over the
//                          lit ones that moves on every frame. Local lights: the 8 with the largest unshadowed
//                          irradiance on the cell (the reference's lights per tile; flag bit 3: one more, drawn from the
//                          rest by its share, so no light is dropped - not the reference's), one point on each, one
//                          shadow ray each. The sun: one shadow ray. Indirect: the 3 x 3 probes around the cell (on its
//                          face), weighted by distance and by how far they are off the cell's plane.
//   SurfaceCacheProbesGen  radiosity. A probe traces 4 x 4 cosine-stratified rays; a ray takes the final lighting of the
//                          cell it meets (its largest channel held to the header's cap in exposed units; the cell is
//                          marked: it is in use) or the sky; the probe's irradiance is a running mean over at most the
//                          header's probe frames.
//                          With flag bit 4 (surface_cache.direct_stochastic; the reference's stochastic direct lighting,
//                          off by default there too) the local lights are not the 8 strongest but one sample of all
//                          the cell's lights by A's world-point sampler (MegaLightsWorld.hlsli: the light drawn by its
//                          unshadowed contribution, area lights by their integral), one shadow ray, and the cell keeps
//                          a running mean of it over at most P[4].x frames.
//                          With flag bit 6 (surface_cache.direct_analytic; the reference's default path) each of the 8
//                          lights gives its unshadowed irradiance by its integral over the light (A's
//                          mlLightUnshadowed: the same value every update) times one shadow ray to the light's centre -
//                          no random point on the light, so a cell's value does not change between updates and
//                          neighbouring cells agree (area lights cast hard shadows in the cache, as in the
//                          reference). Without it: one random point on each light, one shadow ray to it.
// P[4].z = the first thread index of the dispatch (SurfaceCacheCellsGen: the frame's budget goes in several dispatches).
// P[0] = { cache UAV, budget, frame, flags (bit 0: local lights and sun, bit 1: radiosity, bit 3: the remainder light,
//          bit 4: stochastic local lights, bit 5: feedback order, bit 6: analytic local lights; diagnostics
//          (surface_cache.debug_skip, to find which part of the cell lighting a fault is in): bit 7 no local lights,
//          bit 8 no sun, bit 9 the lights are chosen but not evaluated, bit 10 the lights are evaluated without their
//          shadow rays); bit 11 the cell's shadow rays (lights, sun) take alpha-tested casters as opaque; bit 12
//          (diagnostics) the lights' shadow rays under the GI mask; bit 13 the lights' shadow rays as inline queries
//          after all the lights are evaluated (surface_cache.direct_shadow_inline) }, P[4] = { asuint(stochastic max frames), asuint(min sample weight), 0, 0 }
// P[1].xyz = constant sky radiance (SKY1), P[1].w = ray length, P[2] = atmosphere SRVs (SKY0), P[3].xyz = constant sun
// illuminance (SKY1) (GiSky.hlsli), P[3].w = seed bounces; P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/Shading/MegaLightsWorld.hlsli"
#include "Passes/SurfaceCache/SurfaceCache.hlsli"

#define SC_LIGHTS_PER_CELL 8u

// The reflectance a diffuse bounce carries: the diffuse colour and a fully rough share of the specular colour.
float3 scAlbedoOf(GpuMaterial m)
{
    const float3 diffuse = m.baseColor * (1 - m.metallic);
    const float3 specular = lerp(float3(0.04, 0.04, 0.04), m.baseColor, m.metallic);
    return saturate(diffuse + 0.45 * specular);
}

// Whether a ray may be launched: finite origin, a finite direction of unit length. (Traversal of a ray with a NaN or an
// infinite component is undefined: every ray of this library is checked - a cell or a probe holds a point other passes
// wrote.)
bool scRayOk(float3 origin, float3 direction)
{
    const float d = dot(direction, direction);
    return all(abs(origin) < 1e9) && d > 0.98 && d < 1.02;  // (comparisons are false for NaN)
}
// ... and its interval: 0 <= TMin <= TMax, both finite (max(a, NaN) is not defined to be a).
bool scRayOk(RayDesc ray) { return scRayOk(ray.Origin, ray.Direction) && ray.TMin >= 0 && ray.TMax >= ray.TMin && ray.TMax < 1e30; }

// A sampled light point's shadow ray under the GI mask (the sampled-point direct light: surface_cache.direct_analytic=false).
bool scShadowVisible(RtSceneSrvs scene, RayDesc ray) { return scRayOk(ray.Origin, ray.Direction) && rtVisible(scene, ray, RT_MASK_GI); }

// An orthonormal frame around n (Duff et al. 2017).
void scFrame(float3 n, out float3 t, out float3 bt)
{
    const float sg = n.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (sg + n.z);
    const float c = n.x * n.y * a;
    t = float3(1 + sg * n.x * n.x * a, sg * c, -sg * n.x);
    bt = float3(c, sg + n.y * n.y * a, -n.y);
}
float3 scCosineDirection(float3 n, float2 u)
{
    float3 t, bt;
    scFrame(n, t, bt);
    const float r = sqrt(u.x), phi = 6.28318530718 * u.y;
    return normalize(t * (r * cos(phi)) + bt * (r * sin(phi)) + n * sqrt(max(1 - u.x, 0.0)));
}

// The surface a ray met: marked in the cache (its cell stays in use, and exists from now on) and read. False when the
// ray met the inside of closed geometry or an analytic emitter (nothing to mark).
bool scMeet(RWByteAddressBuffer b, ScLayout l, RtSceneSrvs scene, RtHit hit, RayDesc ray, out float3 position, out float3 face, out ScSample cell)
{
    position = face = 0;
    cell = (ScSample)0;
    if (hit.instance == RT_INSTANCE_EMITTER) return false;
    const RtSurface s = rtSurface(scene, hit, ray.Origin, ray.Direction);
    GpuMaterial m = loadMaterial(s.material);
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return false;
    position = s.position;
    face = dot(s.geometricNormal, ray.Direction) > 0 ? -s.geometricNormal : s.geometricNormal;
    m = rtHitMaterial(m, s, scCellSize(l, s.position), dot(s.normal, ray.Direction));
    scMarkQuiet(b, l, position, face, scAlbedoOf(m), m.emissive);
    cell = scRead(b, l, position, face);
    return true;
}

// Whether the centre of a light is seen from x (n: the surface's normal): the reference's shadow ray of a card texel.
// The ray ends 5 cm before the light's surface (S's rule: what lies within 5 cm of a light casts no shadow).
// (flags: RAY_FLAG_FORCE_OPAQUE with surface_cache.shadow_rays_opaque - the reference's hardware rays run no alpha
// masking by default (r.Lumen.HardwareRayTracing.SurfaceCacheAlphaMasking 0): a ray's cost is the traversal's alone.)
bool scCentreVisible(RtSceneSrvs scene, float3 x, float3 n, GpuLight g, float bias, uint flags)
{
    const float3 d = g.position - x;
    const float dist = length(d);
    if (!(dist > 0)) return true;
    const uint type = lightType(g);
    const float radius = type == LIGHT_SPHERE ? g.size.x : type == LIGHT_TUBE ? g.size.y : 0.0;
    const float3 wi = d / dist;
    const float reach = dist - radius - 0.05;
    RayDesc ray;
    ray.Origin = x + n * (dot(n, wi) < 0 ? -bias : bias);
    ray.Direction = wi;
    ray.TMin = bias;
    ray.TMax = reach > bias ? reach : bias;  // (a NaN reach - a light record without a valid size - gives the bias)
    if (!scRayOk(ray)) return false;
    return rtVisible(scene, ray, (flags & 0x80000000u) != 0 ? RT_MASK_GI : RT_MASK_SHADOW, flags & 0x7FFFFFFFu);
}

// Visibility by an inline ray query (surface_cache.direct_shadow_inline): no hit groups and no continuation of the ray
// generation shader across the trace - the query runs to its end inside this function. Non-opaque candidates take the
// alpha test here, at most SC_INLINE_CANDIDATES of them a TLAS (more: the ray counts as blocked - the cap only stops
// a ray that would not end). The emitters' boxes are never committed (a light has no body).
#define SC_INLINE_CANDIDATES 64u
bool scVisibleInline(RtSceneSrvs scene, RayDesc ray, uint mask, uint flags)
{
    if (!scRayOk(ray)) return false;
    [loop] for (uint tlas = 0; tlas < 2; ++tlas)
    {
        RaytracingAccelerationStructure structure = ResourceDescriptorHeap[tlas == 0 ? scene.tlasStatic : scene.tlasDynamic];
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(structure, flags, mask, ray);
        uint candidates = 0;
        [loop] while (q.Proceed())
        {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
            if (++candidates > SC_INLINE_CANDIDATES) return false;
            if (rtAlphaOpaque(scene, q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
                q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() != COMMITTED_NOTHING) return false;
    }
    return true;
}

// The shadow ray of a light's centre (scCentreVisible's ray); false when the light's place gives no ray.
bool scCentreRay(float3 x, float3 n, GpuLight g, float bias, out RayDesc ray)
{
    ray.Origin = x;
    ray.Direction = n;
    ray.TMin = ray.TMax = bias;
    const float3 d = g.position - x;
    const float dist = length(d);
    if (!(dist > 0)) return false;
    const uint type = lightType(g);
    const float radius = type == LIGHT_SPHERE ? g.size.x : type == LIGHT_TUBE ? g.size.y : 0.0;
    const float3 wi = d / dist;
    const float reach = dist - radius - 0.05;
    ray.Origin = x + n * (dot(n, wi) < 0 ? -bias : bias);
    ray.Direction = wi;
    ray.TMax = reach > bias ? reach : bias;
    return scRayOk(ray);
}

// The item a thread of a lighting pass works on: the entries not lit yet first, then a window over the lit ones.
uint scPick(RWByteAddressBuffer b, uint index, uint budget, uint frame, uint lit, uint fresh, uint count, uint listBase)
{
    if (index < fresh) return b.Load(listBase + (count - 1 - index) * 4);
    const uint j = index - fresh;
    if (j >= lit) return SC_NONE;
    const uint room = budget - min(fresh, budget);
    const uint at = lit > room ? (j + (frame % lit) * (room % lit)) % lit : j;
    return b.Load(listBase + at * 4);
}

// The cell a thread of the direct-light pass works on: the cells not lit yet, then - with flag bit 5,
// surface_cache.lighting_feedback - half of the room left for the cells consumers read since the last upkeep (a window
// over that list when it is longer), then the window over all lit cells.
uint scPickCell(RWByteAddressBuffer b, uint index, uint budget, uint frame, uint n, bool feedback)
{
    const uint lit = b.Load(8), fresh = b.Load(12);
    if (!feedback || index < fresh) return scPick(b, index, budget, frame, lit, fresh, n, scListOffset(n, 0));
    const uint wanted = b.Load(52);
    const uint room = budget - min(fresh, budget);
    const uint share = min(room / 2, wanted);
    const uint j = index - fresh;
    if (j < share)
    {
        const uint at = wanted > share ? (j + (frame % wanted) * (share % wanted)) % wanted : j;
        return b.Load(scFeedbackListOffset(n, at));
    }
    return scPick(b, index - share, budget - share, frame, lit, fresh, n, scListOffset(n, 0));
}

[shader("raygeneration")]
void SurfaceCacheSeedGen()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const ScLayout l = scLayout(b);
    if (l.entries == 0) return;
    const RtSceneSrvs scene = rtScene();
    uint seed = giRandom(DispatchRaysIndex().x * 7919u + P[0].z * 15485863u + 3u);
    const float z = 1 - 2 * giUnit(seed), phi = 6.28318530718 * giUnit(seed + 1);
    const float r = sqrt(max(1 - z * z, 0.0));
    RayDesc ray;
    ray.Origin = l.camera;
    ray.Direction = float3(r * cos(phi), r * sin(phi), z);
    ray.TMin = 0;
    ray.TMax = giRayLength();
    const uint bounces = P[3].w;
    [loop] for (uint depth = 0; depth <= bounces; ++depth)
    {
        if (!scRayOk(ray.Origin, ray.Direction)) return;
        const RtHit hit = rtTraceClosest(scene, ray, RAY_FLAG_NONE, RT_MASK_GI);
        if (hit.t < 0) return;
        float3 position, face;
        ScSample cell;
        if (!scMeet(b, l, scene, hit, ray, position, face, cell)) return;
        seed = giRandom(seed + 2);
        ray.Origin = position + face * (1e-3 + 2e-4 * distance(position, l.camera));
        ray.Direction = scCosineDirection(face, float2(giUnit(seed), giUnit(seed + 1)));
    }
}

[shader("raygeneration")]
void SurfaceCacheCellsGen()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const ScLayout l = scLayout(b);
    const uint n = l.entries;
    if (n == 0) return;
    // (P[4].z: the first thread of this dispatch - the cells of a frame go in bands, ReflectionSystem.cpp kCellBand)
    const uint slot = scPickCell(b, DispatchRaysIndex().x + P[4].z, P[0].y, P[0].z, n, (P[0].w & 32u) != 0);
    if (slot == SC_NONE) return;
    const uint4 data = b.Load4(scDataOffset(n, slot));
    const float3 position = asfloat(data.xyz), normal = scUnpackOct(data.w);
    if (!scRayOk(position, normal)) return;  // (a cell without a valid point: no rays from it)
    const RtSceneSrvs scene = rtScene();
    const float bias = 1e-3 + 2e-4 * distance(position, l.camera);
    const float size = scCellSize(l, position);
    const uint seed = giRandom(slot * 9781u + P[0].z * 6271u + 17u);
    const uint3 before = b.Load3(scLightOffset(n, slot));
    float3 direct = scUnpackRgb(before.x), sun = scUnpackRgb(before.y), indirect = scUnpackRgb(before.z);
    // (bit 12, diagnostics - surface_cache.debug_skip 16: the lights' shadow rays under the GI mask; carried in bit 31)
    const uint shadowFlags = (P[0].w & 2048u) != 0 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    const uint lightRayFlags = shadowFlags | ((P[0].w & 4096u) != 0 ? 0x80000000u : 0u);
    if (P[0].w & 1u)
    {
        const float3 directBefore = direct;
        direct = 0;
        sun = 0;
        if (P[0].w & 16u)
        {
            const MlPoint p = mlPointLambert(position, normal, float3(1, 1, 1));  // (its radiance is irradiance / pi)
            const MlWorldSamples samples = mlWorldSamples(scene, p, 1, giUnit(seed), asfloat(P[4].y), g_exposure, UNX_NONE);
            [loop] for (uint i = 0; i < samples.count; ++i)
            {
                const bool visible = !samples.castShadow[i] ||
                                     mlSampleVisible(scene, position, normal, samples.light[i], float2(giUnit(seed + 1), giUnit(seed + 2)), bias, bias, 0.05);
                if (visible) direct += 3.14159265 * mlLightUnshadowed(p, loadLight(samples.light[i]), samples.light[i], UNX_NONE) * samples.weight[i];
            }
            // the running mean (frames in the head's bits 24-31)
            const uint headOffsetS = scHeadsOffset(n, slot);
            const uint hadS = b.Load(headOffsetS) >> 24;
            const float framesS = min((float)hadS + 1, max(asfloat(P[4].x), 1.0));
            if (framesS > 1) direct = lerp(directBefore, direct, 1 / framesS);
            if ((float)hadS < framesS) b.InterlockedAdd(headOffsetS, 1u << 24);
        }
        else if (scene.pad != 0xFFFFFFFFu && (P[0].w & 128u) == 0)
        {
            // the lights of the cell's place in the light grid, the strongest SC_LIGHTS_PER_CELL kept
            g_rtLightData = scene.pad;
            ByteAddressBuffer lights = ResourceDescriptorHeap[scene.pad];
            const RtLightGrid grid = lights.Load<RtLightGrid>(0);
            const uint gridCell = rtLightCell(grid, position);
            uint chosen[SC_LIGHTS_PER_CELL];
            float weight[SC_LIGHTS_PER_CELL];
            uint held = 0;
            float total = 0;
            if (gridCell != ~0u)
            {
                const uint k0 = rtLightCellStart(gridCell), k1 = rtLightCellStart(gridCell + 1);
                [loop] for (uint k = k0; k < k1; ++k)
                {
                    const uint li = rtLightCellLight(k);
                    const float w = rtLightOrientedImportance(rtLightFetch(li), position, normal, false);
                    if (!(w > 0)) continue;
                    total += w;
                    // insertion by weight, the weakest falls out
                    uint at = held;
                    if (held == SC_LIGHTS_PER_CELL)
                    {
                        if (w <= weight[SC_LIGHTS_PER_CELL - 1]) continue;
                        at = SC_LIGHTS_PER_CELL - 1;
                    }
                    else ++held;
                    [loop] while (at > 0 && weight[at - 1] < w)
                    {
                        weight[at] = weight[at - 1];
                        chosen[at] = chosen[at - 1];
                        --at;
                    }
                    weight[at] = w;
                    chosen[at] = li;
                }
            }
            float kept = 0;
            const bool analytic = (P[0].w & 64u) != 0;
            const MlPoint lambert = mlPointLambert(position, normal, float3(1, 1, 1));  // (its radiance is irradiance / pi)
            uint evaluated = 0;
            if (analytic && (P[0].w & 8192u) != 0)
            {
                // surface_cache.direct_shadow_inline: the lights are evaluated first, in a loop of a fixed count with no
                // ray in it; then the rays, in a second loop of the same fixed count, each an inline query. Nothing but
                // the eight irradiances and the running sum lives across a trace, and no trace suspends this shader.
                float3 lightE[SC_LIGHTS_PER_CELL];
                uint lightOf[SC_LIGHTS_PER_CELL];
                [loop] for (uint a = 0; a < SC_LIGHTS_PER_CELL; ++a)
                {
                    lightE[a] = 0;
                    lightOf[a] = a < held ? chosen[a] : 0u;
                    if (a >= held) continue;
                    kept += weight[a];
                    lightE[a] = 3.14159265 * mlLightUnshadowed(lambert, loadLight(chosen[a]), chosen[a], UNX_NONE);
                    if (!all(lightE[a] >= 0) || !all(lightE[a] < 1e30)) lightE[a] = 0;  // (NaN, infinite: no light)
                }
                [loop] for (uint r = 0; r < SC_LIGHTS_PER_CELL; ++r)
                {
                    if (!any(lightE[r] > 0)) continue;
                    const GpuLight g = loadLight(lightOf[r]);
                    bool visible = true;
                    if (lightCastsShadow(g) && (P[0].w & 1024u) == 0)
                    {
                        RayDesc ray;
                        visible = scCentreRay(position, normal, g, bias, ray) &&
                                  scVisibleInline(scene, ray, (lightRayFlags & 0x80000000u) != 0 ? RT_MASK_GI : RT_MASK_SHADOW, lightRayFlags & 0x7FFFFFFFu);
                    }
                    if (visible) direct += lightE[r];
                }
                evaluated = held;  // (done: the loop below has nothing left; the kept lights stay known to the remainder draw)
            }
            [loop] for (uint i = evaluated; i < held; ++i)
            {
                kept += weight[i];
                if (P[0].w & 512u) continue;  // (diagnostics: chosen, not evaluated)
                if (analytic)
                {
                    const GpuLight g = loadLight(chosen[i]);
                    const float3 e = 3.14159265 * mlLightUnshadowed(lambert, g, chosen[i], UNX_NONE);
                    if (any(e > 0) && ((P[0].w & 1024u) != 0 || !lightCastsShadow(g) || scCentreVisible(scene, position, normal, g, bias, lightRayFlags))) direct += e;
                    continue;
                }
                RtLocalChoice c;
                c.valid = true;
                c.li = chosen[i];
                c.probability = 1;
                const RtLocalSample ls = rtLocalLightFinish(scene, c, position, giUnit(seed + 4 + 2 * i), giUnit(seed + 5 + 2 * i), size);
                if (!ls.valid) continue;
                const float mu = dot(normal, ls.wi);
                if (mu > 0 && (!ls.castShadow || scShadowVisible(scene, rtLocalShadowRay(position, normal, ls, bias)))) direct += ls.weight * mu;
            }
            if ((P[0].w & 8u) != 0 && gridCell != ~0u && total > kept * (1 + 1e-6))
            {
                // one light of the rest, drawn by its share of what the kept ones leave
                const float target = giUnit(seed + 40) * (total - kept);
                const uint k0 = rtLightCellStart(gridCell), k1 = rtLightCellStart(gridCell + 1);
                float run = 0, share = 0;
                uint pick = ~0u;
                [loop] for (uint k = k0; k < k1; ++k)
                {
                    const uint li = rtLightCellLight(k);
                    bool isKept = false;
                    for (uint i = 0; i < held; ++i) isKept = isKept || chosen[i] == li;
                    if (isKept) continue;
                    const float w = rtLightOrientedImportance(rtLightFetch(li), position, normal, false);
                    if (!(w > 0)) continue;
                    run += w;
                    pick = li;
                    share = w;
                    if (run > target) break;
                }
                if (pick != ~0u)
                {
                    RtLocalChoice c;
                    c.valid = true;
                    c.li = pick;
                    c.probability = share / (total - kept);
                    const RtLocalSample ls = rtLocalLightFinish(scene, c, position, giUnit(seed + 41), giUnit(seed + 42), size);
                    const float mu = ls.valid ? dot(normal, ls.wi) : 0;
                    if (mu > 0 && (!ls.castShadow || scShadowVisible(scene, rtLocalShadowRay(position, normal, ls, bias)))) direct += ls.weight * mu;
                }
            }
        }
        const float3 toSun = giSunDirection(seed + 3);
        const float muS = dot(normal, toSun);
        if (muS > 0 && (P[0].w & 256u) == 0)
        {
            const float3 e = giSunIlluminance(position);
            if (any(e > 0))
            {
                RayDesc r;
                r.Origin = position + normal * bias;
                r.Direction = toSun;
                r.TMin = 0;
                r.TMax = giRayLength();
                if (scRayOk(r) && rtVisible(scene, r, RT_MASK_GI, shadowFlags)) sun = e * muS;
            }
        }
    }
    if (P[0].w & 2u)
    {
        // the 3 x 3 probes around the cell on its face
        const uint level = scLevel(l, position);
        const uint face = scFace(normal);
        const int3 centre = scCoord(level, position, SC_PROBE_SPACING);
        const int3 du = face < 2 ? int3(0, 1, 0) : int3(1, 0, 0), dv = face < 4 ? int3(0, 0, 1) : int3(0, 1, 0);
        const float reach = 1.5 * scLevelSize(level) * SC_PROBE_SPACING;
        const uint probes = scProbeCount(n);
        float3 sum = 0;
        float weights = 0;
        [loop] for (int k = 0; k < 9; ++k)
        {
            const uint probe = scFind(b, scProbeKeysOffset(n, 0), probes, scKeyAt(level, centre + du * (k % 3 - 1) + dv * (k / 3 - 1), face));
            if (probe == SC_NONE) continue;
            if (((b.Load(scProbeHeadsOffset(n, probe)) >> 8) & 0xFFu) == 0) continue;  // not lit yet
            const uint4 pd = b.Load4(scProbeDataOffset(n, probe));
            const float3 offset = asfloat(pd.xyz) - position;
            const float off = abs(dot(offset, normal));
            const float w = saturate(1 - length(offset - normal * dot(offset, normal)) / reach) * exp2(-8.0 * off / reach) * saturate(dot(normal, scUnpackOct(pd.w)));
            if (!(w > 0)) continue;
            sum += w * scUnpackRgb(b.Load(scProbeLightOffset(n, probe)));
            weights += w;
        }
        if (weights > 0) indirect = sum / weights;
    }
    if (any(isnan(direct)) || any(isinf(direct)) || any(isnan(sun)) || any(isinf(sun)) || any(isnan(indirect)) || any(isinf(indirect))) return;
    b.Store3(scLightOffset(n, slot), uint3(scPackRgb(direct), scPackRgb(sun), scPackRgb(indirect)));
    const uint headOffset = scHeadsOffset(n, slot);
    if (((b.Load(headOffset) >> 8) & 0xFFu) == 0) b.InterlockedAdd(headOffset, 1u << 8);  // lit (atomic: marks set bit 0 of this word)
}

[shader("raygeneration")]
void SurfaceCacheProbesGen()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const ScLayout l = scLayout(b);
    const uint n = l.entries;
    if (n == 0) return;
    const uint probes = scProbeCount(n);
    const uint slot = scPick(b, DispatchRaysIndex().x, P[0].y, P[0].z, b.Load(32), b.Load(36), probes, scProbeListOffset(n, 0));
    if (slot == SC_NONE) return;
    const uint4 data = b.Load4(scProbeDataOffset(n, slot));
    const float3 position = asfloat(data.xyz), normal = scUnpackOct(data.w);
    if (!scRayOk(position, normal)) return;  // (a probe without a valid point: no rays from it)
    const RtSceneSrvs scene = rtScene();
    const float3 origin = position + normal * (1e-3 + 2e-4 * distance(position, l.camera));
    const uint seed = giRandom(slot * 9781u + P[0].z * 6271u + 29u);
    const float cap = asfloat(b.Load(44));
    float3 sum = 0;
    [loop] for (uint i = 0; i < 16; ++i)
    {
        const float2 u = (float2(i & 3u, i >> 2) + float2(giUnit(seed + 2 * i), giUnit(seed + 1 + 2 * i))) * 0.25;
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = scCosineDirection(normal, u);
        ray.TMin = 0;
        ray.TMax = giRayLength();
        if (!scRayOk(ray.Origin, ray.Direction)) continue;
        const RtHit hit = rtTraceClosest(scene, ray, RAY_FLAG_NONE, RT_MASK_GI);
        float3 radiance = 0;
        if (hit.t < 0) radiance = giSkyRadiance(ray.Direction);
        else
        {
            float3 at, face;
            ScSample cell;
            if (scMeet(b, l, scene, hit, ray, at, face, cell) && cell.valid) radiance = scFinalLighting(cell);
        }
        const float brightest = max(radiance.r, max(radiance.g, radiance.b)) * g_exposure;
        if (cap > 0 && brightest > cap) radiance *= cap / brightest;
        sum += radiance;
    }
    float3 e = 3.14159265 * sum / 16.0;  // E = pi x the mean radiance of cosine-distributed rays
    const uint headOffset = scProbeHeadsOffset(n, slot);
    const uint had = (b.Load(headOffset) >> 8) & 0xFFu;
    const float frames = min((float)had + 1, max(asfloat(b.Load(48)), 1.0));
    if (frames > 1) e = lerp(scUnpackRgb(b.Load(scProbeLightOffset(n, slot))), e, 1 / frames);
    if (any(isnan(e)) || any(isinf(e))) return;
    b.Store(scProbeLightOffset(n, slot), scPackRgb(max(e, 0.0)));
    if ((float)had < frames) b.InterlockedAdd(headOffset, 1u << 8);
}
