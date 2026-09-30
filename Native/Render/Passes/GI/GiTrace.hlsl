// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1 SPLIT=0,1
// World radiance cache update rays (ARCHITECTURE 2.5; DispatchRays with alpha any-hit, 1.3-5). The fixed per-frame ray
// budget is spent as whole-hemisphere updates: 64 threads per updated entry (GiSelect's stalest-first selection, then
// background entries), thread = texel, direction jittered inside the texel. The hit's outgoing radiance =
// the full model at the hit (rtHitRadiance, as reflection hits): emission + diffuse albedo / pi x (direct sun at the hit
// point, one exact shadow ray within the solar disk + indirect irradiance of the hit's cell) + the specular lobe of the
// sun and of the cell's incident radiance from the mirror direction. The indirect part is the second and later bounces, a smooth field: its cell
// is gi.hit_cell_footprint_scale x the ray footprint (texel cone ~0.36 t) coarse, and is requested for update next frame.
// The texel values go to GiIntegrate with two flags (the ray read a bounce term; from a young cell), which
// sets the entry's history weight (GiInternal giHistoryAlpha) and blends texels, map and SH with it.
//
// P[0] = { cache UAV, ray budget (dispatch width), hit cell footprint scale (float bits), 0 }
// Local lights at the hit: one next-event sample (HitLocalLights.hlsli) and a shadow ray; with them the cache's
// irradiance is the indirect light only where the direct local light is shaded analytically (M) or by the hits' own
// sample. Analytic area lights (raytracing.emitters, design 12.4 structure 2): a ray that meets one records radiance 0 in
// the texel and the irradiance samples (the light occludes what is behind it) and the light's radiance in the entry's
// emitter texel (fourth samples block, GiCache.hlsli giEmitterOffset). Ray hits read the texels alone, their NEE sample
// shades the light (once, at its true shape: the emitter radiance in the texels read by glossy hits drew texel squares
// and counted the light's specular twice); the K path's maps and the light-loop-free readers add the emitter texels
// (M leaves the stable lights' specular to the reflection paths: the interior's -3.47 ms lever, ARCHITECTURE 2.13).
// Emissive meshes (FEATURES_GAME 12 (ii)): besides its texel ray every thread draws one point of RayScene's emissive
// triangles from the anchor (HitLocalLights.hlsli) and both estimates of the irradiance are combined by the balance
// heuristic: the texel rays sample directions with density q = |p|^3 / 2 per sr (uniform in the hemispherical
// octahedral map), the emitter samples with p_l; power heuristic (beta 2, one sample of each per thread): a texel ray's
// emission counts with q^2 / (q^2 + p_l^2), an emitter sample with p_l^2 / (q^2 + p_l^2) / p_l (it goes to the irradiance
// samples after the texel ones, GiIntegrate). A large dim emitter (small p_l) then stays with the texel rays, which see it
// with little variance (the balance heuristic added light-sampling noise there [measured: reflection furnace, 6 of 11
// runs failed against 2 of 11]); a small bright one (large p_l) goes to the emitter samples. The texels (the K path's
// radiance) keep the full emission.
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli: SKY0 atmosphere LUTs, SKY1 constants), ray length; P[3].w = gi.experiment_disable
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view (sun, scene buffers).
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"

// Texel cone of an 8 x 8 hemispherical texel (2 pi / 64 sr ~ 10.1 deg half-angle): footprint diameter ~0.36 t.
#define GI_FOOTPRINT_PER_METRE 0.36

float giBias(GiHeader h, float3 p) { return 1e-3 + 2e-4 * distance(p, h.camera); }

// Redesign V2 P1 (gi.hit_light_footprint, GI_P1_FLAGS bit 3): a texel holds the mean over its ray cone of the radiance
// leaving the hits toward the anchor (the specular lobe is widened by the cone below for the same reason). A point or spot
// light's diffuse term at one hit point is 1 / d^2 there: GI rays landing in a lamp's hotspot (a ceiling 10 cm above a
// bulb) gave rare samples hundreds of times the mean - in the bathhouse 44 % of the converged GI energy came from hits
// whose local term exceeded 1000 nit (a diagnostic cap), and new cells stayed spotted for seconds. The diffuse term is
// instead the exact mean of the light's irradiance over the ray's footprint on the hit plane: the texel cone's square
// cross-section (side GI_FOOTPRINT_PER_METRE x t) projected along the ray onto the plane, mean E = I x Omega / area, Omega
// the footprint quadrilateral's solid angle seen from the light (Van Oosterom and Strackee 1983, two triangles). Returned
// as the factor on the point value, I cos / d^2 (1 when the footprint vanishes: exact in the limit). The light's window,
// spot factor, choice and shadow ray stay those of the hit point (smooth over the footprint, or the visibility estimate
// the cell averages as before).
float giTriangleSolidAngle(float3 a, float3 b, float3 c)
{
    const float la = length(a), lb = length(b), lc = length(c);
    const float num = dot(a, cross(b, c));
    const float den = la * lb * lc + dot(a, b) * lc + dot(a, c) * lb + dot(b, c) * la;
    return 2 * atan2(num, den);
}
float giPointFootprintScale(float3 light, float3 x, float3 n, float3 dir, float t, float sizeScale)
{
    const float a = t * GI_FOOTPRINT_PER_METRE * sizeScale, dn = dot(n, dir);
    const float3 d = light - x;
    const float d2 = dot(d, d), cosx = dot(n, d) * rsqrt(max(d2, 1e-12));
    if (!(a > 0) || abs(dn) < 1e-3 || !(cosx > 0) || !(d2 > 0)) return 1;
    float3 u, v;
    giBasis(dir, u, v);
    float3 c[4];
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float3 p = x + (u * ((k & 1) ? 0.5 : -0.5) + v * ((k & 2) ? 0.5 : -0.5)) * a;
        c[k] = p - dir * (dot(n, p - x) / dn) - light;  // on the hit plane, relative to the light
    }
    // corners 0 (-,-), 1 (+,-), 3 (+,+), 2 (-,+): the quadrilateral in order
    const float omega = abs(giTriangleSolidAngle(c[0], c[1], c[3]) + giTriangleSolidAngle(c[0], c[3], c[2]));
    const float area = a * a / abs(dn);
    return (omega / area) / (cosx / d2);
}
// Density of the texel rays at direction l (anchor frame, unit): uniform in the hemispherical octahedral map's uv, whose
// point p = l / (|l.x| + |l.y| + l.z) has dw / (du dv) = 2 / |p|^3.
float giTexelDensity(float3 l) { const float s = abs(l.x) + abs(l.y) + l.z; return 0.5 / (s * s * s); }

[shader("raygeneration")]
void GiTraceGen()
{
    const uint thread = DispatchRaysIndex().x;
    if (thread >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    uint entry;
    bool background;
    if (!giUpdateSlot(b, h, thread / GI_TEXEL_COUNT, entry, background)) return;
    const uint texel = thread % GI_TEXEL_COUNT;
    g_rtHitCone = 0.5 * GI_FOOTPRINT_PER_METRE;  // A9 coat lobes at the hits: widened by the texel cone (HitShading rtHitCoat)

    const RtSceneSrvs scene = rtScene();
    const float3 anchor = giAnchorPosition(b, h, entry);
    const float3 n = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(n, t, bt);
    const uint shAddress = h.offSh + entry * GI_SH_STRIDE;
    // gi.deterministic (P[0].w bit 0): seeds from the entry's key, not its index (allocation order).
    const uint identity = (P[0].w & 1u) != 0 ? giDetKey(b, h, entry) : entry;
    const uint seed = giRandom(identity * 9781u + h.frame * 6271u + texel * 26699u);
    // The point inside the texel: the entry's successive updates walk the R2 sequence (Roberts 2018) from a per-texel
    // rotation, so the history (a running mean over <= history_updates_max updates) averages well-spread points of every
    // texel instead of independent ones: same rays, lower error (the texel's integrand is smooth or has one edge, where
    // independent jitter converges as 1/sqrt(N) and a low-discrepancy set faster). Experiment bit 64: independent jitter.
    float2 jitter;
    if ((P[3].w & 64u) != 0) jitter = float2(giUnit(seed), giUnit(seed + 1));
    else
    {
        const uint rotation = giRandom(identity * 9781u + texel * 26699u + 0x9E3779B9u);
        const uint updates = b.Load(shAddress + GI_SH_UPDATES);
        jitter = frac(float2(giUnit(rotation), giUnit(rotation + 1)) + (float)(updates & 0xFFFFu) * float2(0.7548776662, 0.5698402910));
    }
    const float2 uv = (float2(texel % GI_TEXELS, texel / GI_TEXELS) + jitter) / GI_TEXELS;
    const float3 local = giHemiOctDecode(uv);
    RayDesc r;
    r.Origin = anchor + n * giBias(h, anchor);
    if ((P[3].w & 65536u) != 0)
    {
        // Experiment 65536 (diagnostic, redesign V2.2 P1'-b cause search): each ray from a uniform point of the cell's
        // tangent square around the anchor (the cell's area mean instead of its anchor point's irradiance; points past the
        // surface's edge are not corrected) - does the cached level depend on where the cell's anchor happened to land?
        const float cell = giCellSize(h, b.Load(h.offMeta + entry * 16) & 31u);
        r.Origin += (t * (giUnit(seed + 40) - 0.5) + bt * (giUnit(seed + 41) - 0.5)) * cell;
    }
    r.Direction = normalize(t * local.x + bt * local.y + n * local.z);
    r.TMin = 0;
    r.TMax = giRayLength();
    RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER);
    // A back face closer than the origin's own offset is a surface the origin lies on, not closed geometry around it:
    // anchors on a crease (a hit exactly on the edge where two faces meet) start their rays on the other face's plane, and
    // counting those as "inside" (radiance 0) turned such cells black (a live ceiling cell on a furnace room's edge read
    // 1.6 % of its true irradiance). The ray continues from just past that plane.
    const float onSurface = 2 * giBias(h, anchor);
    if (hit.t >= 0 && hit.t < onSurface && hit.instance != RT_INSTANCE_EMITTER)
    {
        const RtSurface s0 = rtSurface(scene, hit, r.Origin, r.Direction);
        if (!s0.frontFace && (loadMaterial(s0.material).classFlags & MATERIAL_TWO_SIDED) == 0)
        {
            r.TMin = onSurface;
            hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER);
        }
    }

    float3 radiance, sampleRadiance;
    float3 bounceRadiance = 0;  // the cache-fed part of the radiance (SPLIT, and gi.bounce_split: GI_P1_FLAGS bit 4)
#if SPLIT
    float3 nonBounceRadiance = 0;
#endif
    bool readsBounce = false;  // the hit's radiance has a bounce term (the cache's irradiance and mirror radiance read there)
    bool youngBounce = false;  // read from a young cell, or from other levels in place of a cell without data
    float bounceShare = 0;     // luminance share of the radiance that came from the cache reads (GiIntegrate: Jacobi length)
    float3 emissionOut = 0;  // the hit's own emission's share the MIS moves to the emitter samples (1 - w_b) x emission
    bool emitter = false;
    float3 emitterRadiance = 0;  // the analytic area light this ray met (emitter texel)
    float distanceToHit;
    if (hit.t < 0)
    {
        radiance = giSkyRadiance(r.Direction);
        if ((P[3].w & 32) != 0 && r.Direction.y < 0) radiance = 0;  // attribution: nothing below the horizon (the sky LUT's lit ground)
#if SKY != SKY_ATMOSPHERE
        if (r.Direction.y > asfloat(P[4].x)) radiance = 0;  // tests: the constant sky in a band above the horizon (P[4].x = 1: all)
#endif
        distanceToHit = 65000;
    }
    else if (hit.instance == RT_INSTANCE_EMITTER)
    {
        // An analytic area light (raytracing.emitters): 0 in the texel and the irradiance samples, its radiance in the
        // emitter texel (header).
        distanceToHit = hit.t;
        radiance = 0;
        emitterRadiance = rtEmitterCounts(scene.pad, hit.primitive) ? rtEmitterRadiance(hit.primitive, r.Origin) : float3(0, 0, 0);
        emitter = true;
    }
    else
    {
        distanceToHit = hit.t;
        const RtSurface s = rtSurface(scene, hit, r.Origin, r.Direction);
        // Textures at the GI ray's texel-cone footprint (the width its cache cell is sized by).
        GpuMaterial m = loadMaterial(s.material);
        if ((P[3].w & 8) == 0)
        {
            m = rtHitMaterial(m, s, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z), dot(s.normal, r.Direction));
            rtHitDecals(scene, s, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z), m);  // the direct view's decals (A7)
        }
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
        if (!s.frontFace && !twoSided)
        {
            radiance = 0;  // inside closed geometry
        }
        else
        {
            // The hit is shaded with the full model (rtHitRadiance, as reflection hits): diffuse + the specular lobe of
            // the sun and of the cache's incident radiance from the mirror direction. Diffuse alone lost the specular
            // albedo at every bounce (~7 % of a glazed white tile's reflectance), a geometric deficit in bright closed
            // rooms: the bathhouse's indirect light was 64 % of the reference's [measured, 2026-09-27].
            const float3 mirror = reflect(r.Direction, s.normal);
            float3 irradiance = 0, specular = 0;
            bool created;
            const uint bounceLevel = giLevelForSize(h, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z));
            const uint e = giFindOrCreate(b, h, giSurfaceKey(h, s.position, s.normal, bounceLevel), giAnchorAtHit(h, s.position, r.Direction, hit.t), s.normal, created);
            bool known = false;
            if (e != GI_ENTRY_PENDING)
            {
                if (b.Load(h.offHitStamp + e * 4) != h.frame)  // (stamped this frame: touched and requested already, giKeepRead)
                {
                    giTouch(b, h, e);
                    giRequestHit(b, h, e);
                }
                // Experiment 262144 (diagnostic, V2.2 P1'-b): every bounce read through the interpolated level search below
                if (!created && b.Load(h.offSh + e * GI_SH_STRIDE + GI_SH_UPDATES) != 0 && (P[3].w & 262144u) == 0)
                {
                    float unused;
                    irradiance = giShIrradiance(b, h, e, s.normal, unused);
                    // Experiment 131072 (diagnostic, V2.2 P1'-b: the multi-bounce part is 92 % of the reference's): the bounce
                    // read from the cell's irradiance map instead of its L2 SH
                    if ((P[3].w & 131072u) != 0) irradiance = giIrrMapAt(b, h, e, giAnchorNormal(b, h, e), s.normal);
                    const float3 na = giAnchorNormal(b, h, e);
                    float3 ta, ba;
                    giBasis(na, ta, ba);
                    specular = giTexelRadiance(b, h, e, giHemiOctEncode(float3(dot(mirror, ta), dot(mirror, ba), max(dot(mirror, na), 0.0))));
                    known = true;
                }
            }
            // A bounce cell without data yet (new, or not updated since): the same surface's coarser cells hold the best
            // estimate there. Irradiance 0 in its place made every young cell's first updates dark, and readers of those
            // cells (reflection hits land on fresh fine cells all the time) showed it as dark spots.
            float fallbackWeight = 0, fallbackYoung = 0;
            if (!known)
            {
                g_giReadYoung = 0;
                g_giTrackYoung = true;
                float3 sumE, sumL;
                giCacheLevels(b, h, s.position, s.normal, mirror, true, bounceLevel, sumE, sumL, fallbackWeight);  // coarser, then finer levels
                irradiance = fallbackWeight > 0 ? sumE / fallbackWeight : 0;
                specular = fallbackWeight > 0 ? sumL / fallbackWeight : 0;
                fallbackYoung = g_giReadYoung;
                g_giTrackYoung = false;
            }
            const float3 l = normalize(g_sunDirection);
            const float cosSun = dot(s.normal, l);
            RtHitLighting L;
            L.sunIlluminance = 0;
            L.sunVisibility = 0;
            if ((cosSun > 0 || (m.classFlags & 0xFFu) == MATERIAL_FOLIAGE) && (P[3].w & 16) == 0)  // 16 (attribution): no sun at GI hits
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    // One exact shadow ray toward a point of the solar disk: the texel's history integrates the disk
                    // over frames. (S's VSM lookup here measured 0.28 ms more at 4K city than the rays, 375e39d.)
                    RayDesc sr;
                    sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * giBias(h, s.position);  // geometric side (ReflectionHit)
                    sr.Direction = giSunDirection(seed + 7);
                    sr.TMin = 0;
                    sr.TMax = giRayLength();
                    L.sunIlluminance = e0;
                    L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                }
            }
            // Local lights: one next-event sample and its shadow ray (experiment 128: none).
            float3 local = 0;
            if ((P[3].w & 128) == 0 && (P[3].w & 4096) != 0 && scene.pad != 0xFFFFFFFFu)
            {
                // Experiment 4096 (diagnostic, redesign V2 P1 cause search): every light of the hit's cell, each with its own
                // shadow ray - the deterministic estimator of the same sum (no light choice), traced. Scene lights only.
                g_rtLightData = scene.pad;
                ByteAddressBuffer lb = ResourceDescriptorHeap[scene.pad];
                const RtLightGrid grid = lb.Load<RtLightGrid>(0);
                const uint cell = rtLightCell(grid, s.position);
                if (cell != ~0u)
                {
                    GpuMaterial mc = m;
                    const float alpha = modelAlpha(m.roughness), cone = 0.5 * GI_FOOTPRINT_PER_METRE;
                    mc.roughness = sqrt(sqrt(alpha * alpha + cone * cone));
                    const uint k1 = rtLightCellStart(cell + 1);
                    [loop] for (uint k = rtLightCellStart(cell); k < k1; ++k)
                    {
                        const uint li = rtLightCellLight(k);
                        const RtLight l = rtLightFetch(li);
                        RtLightSample ls;
                        if (!rtLightSample(l, s.position, giUnit(seed + 12 + 2 * k), giUnit(seed + 13 + 2 * k), ls) || !(ls.pdf > 0)) continue;
                        RtLocalSample one = (RtLocalSample)0;
                        one.valid = true;
                        one.castShadow = l.castShadow != 0;
                        one.light = li;
                        one.wi = ls.wi;
                        one.distance = ls.distance;
                        one.weight = ls.L / ls.pdf;
                        const float3 f = rtLocalLightBrdfCos(mc, s.normal, -r.Direction, one.wi, (P[3].w & 1024) != 0);
                        if (any(f > 0) && (!one.castShadow || rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, one, giBias(h, s.position)), RT_MASK_GI)))
                            local += f * one.weight;
                    }
                }
            }
            else if ((P[3].w & 128) == 0)
            {
                const RtLocalSample ls = rtLocalLightSample(scene, s.position, giUnit(seed + 11), giUnit(seed + 12), giUnit(seed + 13),
                                                              hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z));  // the hit cell's footprint
                if (ls.valid)
                {
                    // A texel holds the mean over its ray cone (half-angle ~atan(GI_FOOTPRINT_PER_METRE / 2)) of what leaves the
                    // hit toward the anchor: for the specular lobe that mean is the lobe widened by the cone, alpha' =
                    // sqrt(alpha^2 + tan^2). Evaluated at one direction instead, a glossy hit's point-light highlight came in
                    // as rare huge samples that stayed in the cells' means as bright dots (train, 2026-09-27). The sun's
                    // highlight is filtered the same way (rtHitRadiance's pixelAngle).
                    GpuMaterial mc = m;
                    const float alpha = modelAlpha(m.roughness), cone = 0.5 * GI_FOOTPRINT_PER_METRE;
                    mc.roughness = sqrt(sqrt(alpha * alpha + cone * cone));
                    float3 f = rtLocalLightBrdfCos(mc, s.normal, -r.Direction, ls.wi, (P[3].w & 1024) != 0);  // full model (1024: Lambert)
                    const bool audit = (P[3].w & 32768u) != 0;
                    float3 fPoint = f, fFootprint = f;
                    if ((audit || (b.Load(GI_P1_FLAGS) & 8u) != 0) && (m.classFlags & 0xFFu) != MATERIAL_FOLIAGE && dot(s.normal, ls.wi) > 0)
                    {
                        // P1: the diffuse term as its mean over the ray's footprint (giPointFootprintScale; point and spot lights)
                        RtLight lf;
                        if (ls.light < g_lightCount) lf = rtLightFetch(ls.light);
                        else lf = rtFxLight(ls.light - g_lightCount);
                        if (lf.type == kRtLightPoint || lf.type == kRtLightSpot)
                        {
                            const float3 diffuse = m.baseColor * ((1 - m.metallic) / GI_PI) * dot(s.normal, ls.wi);
                            fFootprint += diffuse * (giPointFootprintScale(lf.position, s.position, s.geometricNormal, r.Direction, hit.t, asfloat(b.Load(GI_P1_FOOTPRINT_SCALE))) - 1);
                            if ((b.Load(GI_P1_FLAGS) & 8u) != 0) f = fFootprint;
                        }
                    }
                    const bool visible = (any(f > 0) || (audit && any(fPoint > 0))) &&
                                         (!ls.castShadow || rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, giBias(h, s.position)), RT_MASK_GI));
                    if (visible && any(f > 0))
                        local = f * ls.weight;
                    if (audit)
                    {
                        // Energy audit (GI_AUDIT_SUMS): the same hits, point term against footprint mean, visibility apart
                        const float3 Y = float3(0.2126, 0.7152, 0.0722);
                        const float pt = max(dot(fPoint * ls.weight, Y), 0), fp = max(dot(fFootprint * ls.weight, Y), 0);
                        const uint base = GI_AUDIT_SUMS + (pt > 1000 ? 32u : 0u);
                        uint64_t prev;
                        b.InterlockedAdd64(base + 0, (uint64_t)(pt * 1024), prev);
                        b.InterlockedAdd64(base + 8, (uint64_t)(fp * 1024), prev);
                        if (visible)
                        {
                            b.InterlockedAdd64(base + 16, (uint64_t)(pt * 1024), prev);
                            b.InterlockedAdd64(base + 24, (uint64_t)(fp * 1024), prev);
                        }
                    }
                }
            }
            if ((P[3].w & (8192u | 16384u)) != 0)
            {
                // Experiment 8192 (diagnostic only - a cap is a bias): the local term's luminance capped at 100 nit (16384: 1000 nit), to tell
                // whether the cells' rare bright updates come from hits in local-light hotspots (redesign V2 P1 cause search).
                const float lum = dot(local, float3(0.2126, 0.7152, 0.0722));
                const float cap = (P[3].w & 8192u) != 0 ? 100.0 : 1000.0;
                if (lum > cap) local *= cap / lum;
            }
            if ((P[3].w & 512) != 0) irradiance = specular = 0;  // 512 (diagnostic): one bounce, the reference's --surface-order 1:2
            L.irradiance = irradiance;
            L.specularRadiance = specular;
            L.local = local;
            // The texel cone's angular width filters the sun's highlight (GI_FOOTPRINT_PER_METRE: footprint / distance).
            float3 unbounced;
            if ((P[3].w & 1024) == 0)
            {
                radiance = rtHitRadiance(m, s.normal, -r.Direction, L, GI_FOOTPRINT_PER_METRE);
#if !SPLIT
                if ((b.Load(GI_P1_FLAGS) & 16u) != 0)
#endif
                {
                    // Evaluate the cache-fed RGB terms themselves, not a luminance
                    // fraction or the subtraction of two nearly equal bright values.
                    RtHitLighting bounceLight = (RtHitLighting)0;
                    bounceLight.irradiance = L.irradiance;
                    bounceLight.specularRadiance = L.specularRadiance;
                    GpuMaterial bounceMaterial = m;
                    bounceMaterial.emissive = 0;
                    bounceRadiance = rtHitRadiance(bounceMaterial, s.normal, -r.Direction, bounceLight, GI_FOOTPRINT_PER_METRE);
                }
                L.irradiance = L.specularRadiance = 0;
                unbounced = rtHitRadiance(m, s.normal, -r.Direction, L, GI_FOOTPRINT_PER_METRE);
            }
            else  // 1024 (analytic tests): Lambert hits, the closed forms of GiAnalytic (the v1 model's Schlick lobe has none)
            {
                unbounced = m.emissive + m.baseColor * (1 - m.metallic) / GI_PI * (L.sunIlluminance * max(cosSun, 0.0) * L.sunVisibility) + local;
                radiance = unbounced + m.baseColor * (1 - m.metallic) / GI_PI * irradiance;
                bounceRadiance = m.baseColor * (1 - m.metallic) / GI_PI * irradiance;
            }
#if SPLIT
            nonBounceRadiance = unbounced;
#endif
            const float total = dot(radiance, float3(0.2126, 0.7152, 0.0722));
            bounceShare = total > 0 ? saturate(1 - dot(unbounced, float3(0.2126, 0.7152, 0.0722)) / total) : 0;
            readsBounce = true;  // the specular term reads the cache too (every surface has a specular lobe)
            // A fallback read is young by the young entries' share of its weight (or without data: 0 in place of the light).
            youngBounce = known ? giYoung(b, h, e) : !(fallbackWeight > 0) || fallbackYoung > 0;
            if (any(m.emissive > 0) && (P[3].w & 256) == 0)  // 256 (attribution): no emitter samples, texel rays alone
            {
                RtGeometry g;
                rtResolve(scene, hit, g);
                const float pl = rtEmissivePdf(scene, s.sceneInstance, g, hit.primitive, r.Direction, hit.t);
                if (pl > 0)
                {
                    const float q = giTexelDensity(float3(dot(r.Direction, t), dot(r.Direction, bt), dot(r.Direction, n)));
                    emissionOut = m.emissive * (pl * pl / (q * q + pl * pl));
                }
            }
        }
    }

    // The raw sample for the per-ray irradiance map and SH (GiIntegrate): radiance, coordinates in the hemisphere map.
    RWStructuredBuffer<uint4> samples = ResourceDescriptorHeap[P[4].y];
    sampleRadiance = emitter ? float3(0, 0, 0) : radiance - emissionOut;
    // The emitter sample of this thread (MIS partner of the texel rays); 0 without emissive triangles.
    float3 emitted = 0;
    float3 emitLocal = float3(0, 0, 1);
    {
        const RtEmissiveSample es = rtEmissiveSample(scene, r.Origin, giUnit(seed + 21), giUnit(seed + 22), giUnit(seed + 23));
        if (es.valid && (P[3].w & 256) == 0)
        {
            emitLocal = float3(dot(es.wi, t), dot(es.wi, bt), dot(es.wi, n));
            if (emitLocal.z > 0)
            {
                RayDesc er;
                er.Origin = r.Origin;
                er.Direction = es.wi;
                er.TMin = 0;
                er.TMax = max(es.distance * (1 - 1e-4) - giBias(h, anchor), 0.0);
                const float q = giTexelDensity(emitLocal);
                if (rtVisible(scene, er, RT_MASK_GI)) emitted = es.L * (es.pdf / (q * q + es.pdf * es.pdf));  // x p_l^2 / (q^2 + p_l^2) / p_l
            }
        }
    }
    {
        RWStructuredBuffer<uint4> emitterSamples = ResourceDescriptorHeap[P[4].y];
        emitterSamples[P[0].y + thread] = uint4(asuint(emitted), octEncode(normalize(emitLocal)));
    }
    samples[thread] = uint4(asuint(sampleRadiance), (uint)round(saturate(uv.x) * 65535.0) | ((uint)round(saturate(uv.y) * 65535.0) << 16));
    samples[3 * P[0].y + thread] = uint4(asuint(emitterRadiance), 0);  // the emitter texel's value (GiIntegrate)
#if SPLIT
    if (!readsBounce) nonBounceRadiance = radiance; // sky, emitters, closed back faces
    samples[4 * P[0].y + thread] = uint4(asuint(emitter ? float3(0, 0, 0) : nonBounceRadiance - emissionOut), 0);
    samples[5 * P[0].y + thread] = uint4(asuint(nonBounceRadiance), 0);
    samples[6 * P[0].y + thread] = uint4(asuint(bounceRadiance), 0);
#else
    // gi.bounce_split (V2.2 11.2): the ray's cache-fed radiance, fifth block (GiIntegrate's B part)
    if ((b.Load(GI_P1_FLAGS) & 16u) != 0) samples[4 * P[0].y + thread] = uint4(asuint(emitter ? float3(0, 0, 0) : bounceRadiance), 0);
#endif
    // The texel's value for GiIntegrate (third block of the samples buffer): radiance, hit distance (fp16, >= 0), bit 16 =
    // the ray read a bounce term, bit 17 = from young cells. By count, not by luminance: a cell without data reads 0 (the
    // most biased read has no luminance). Bits 18-29: the bounce share of the radiance (luminance, unorm12).
    samples[2 * P[0].y + thread] = uint4(asuint(radiance), (f32tof16(min(distanceToHit, 65000.0)) & 0xFFFFu) | (readsBounce ? 0x10000u : 0u) |
                                                             (readsBounce && youngBounce ? 0x20000u : 0u) | ((uint)round(bounceShare * 4095.0) << 18));
}
