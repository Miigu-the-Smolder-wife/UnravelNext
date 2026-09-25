// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// World radiance cache update rays (ARCHITECTURE 2.5; DispatchRays with alpha any-hit, 1.3-5). The fixed per-frame ray
// budget is spent as whole-hemisphere updates: 64 threads per updated entry (GiSelect's stalest-first selection, then
// background entries), thread = texel, direction jittered inside the texel. The hit's outgoing radiance =
// emission + diffuse albedo / pi * (direct sun at the hit point, one exact shadow ray within the solar disk
// + indirect irradiance of the hit's cell). The indirect part is the second and later bounces, a smooth field: its cell
// is gi.hit_cell_footprint_scale x the ray footprint (texel cone ~0.36 t) coarse, and is requested for update next frame.
// Texels blend with the entry's history weight (GiInternal giHistoryAlpha).
//
// P[0] = { cache UAV, ray budget (dispatch width), hit cell footprint scale (float bits), 0 }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli: SKY0 atmosphere LUTs, SKY1 constants), ray length; P[3].w = gi.experiment_disable
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view (sun, scene buffers).
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"

// Texel cone of an 8 x 8 hemispherical texel (2 pi / 64 sr ~ 10.1 deg half-angle): footprint diameter ~0.36 t.
#define GI_FOOTPRINT_PER_METRE 0.36

float giBias(GiHeader h, float3 p) { return 1e-3 + 2e-4 * distance(p, h.camera); }

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

    const RtSceneSrvs scene = rtScene();
    const float3 anchor = giAnchorPosition(b, h, entry);
    const float3 n = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(n, t, bt);
    const uint shAddress = h.offSh + entry * GI_SH_STRIDE;
    const uint history = giHistory(b, h, entry);
    const float alpha = giHistoryAlpha(h, history);
    // gi.deterministic (P[0].w bit 0): seeds from the entry's key, not its index (allocation order).
    const uint identity = (P[0].w & 1u) != 0 ? giDetPriority(b, h, entry) : entry;
    const uint seed = giRandom(identity * 9781u + h.frame * 6271u + texel * 26699u);
    const float2 uv = (float2(texel % GI_TEXELS, texel / GI_TEXELS) + float2(giUnit(seed), giUnit(seed + 1))) / GI_TEXELS;
    const float3 local = giHemiOctDecode(uv);
    RayDesc r;
    r.Origin = anchor + n * giBias(h, anchor);
    r.Direction = normalize(t * local.x + bt * local.y + n * local.z);
    r.TMin = 0;
    r.TMax = giRayLength();
    RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI);
    // A back face closer than the origin's own offset is a surface the origin lies on, not closed geometry around it:
    // anchors on a crease (a hit exactly on the edge where two faces meet) start their rays on the other face's plane, and
    // counting those as "inside" (radiance 0) turned such cells black (a live ceiling cell on a furnace room's edge read
    // 1.6 % of its true irradiance). The ray continues from just past that plane.
    const float onSurface = 2 * giBias(h, anchor);
    if (hit.t >= 0 && hit.t < onSurface)
    {
        const RtSurface s0 = rtSurface(scene, hit, r.Origin, r.Direction);
        if (!s0.frontFace && (loadMaterial(s0.material).classFlags & MATERIAL_TWO_SIDED) == 0)
        {
            r.TMin = onSurface;
            hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI);
        }
    }

    float3 radiance;
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
    else
    {
        distanceToHit = hit.t;
        const RtSurface s = rtSurface(scene, hit, r.Origin, r.Direction);
        // Textures at the GI ray's texel-cone footprint (the width its cache cell is sized by).
        GpuMaterial m = loadMaterial(s.material);
        if ((P[3].w & 8) == 0) m = rtHitMaterial(m, s, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z), dot(s.normal, r.Direction));
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
        if (!s.frontFace && !twoSided)
        {
            radiance = 0;  // inside closed geometry
        }
        else
        {
            const float3 albedo = m.baseColor * (1 - m.metallic);
            float3 irradiance = 0;
            bool created;
            const uint bounceLevel = giLevelForSize(h, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z));
            const uint e = giFindOrCreate(b, h, giSurfaceKey(h, s.position, s.normal, bounceLevel), giAnchorAtHit(h, s.position, r.Direction), s.normal, created);
            bool known = false;
            if (e != GI_ENTRY_PENDING)
            {
                giTouch(b, h, e);
                giRequestHit(b, h, e);
                if (!created && b.Load(h.offSh + e * GI_SH_STRIDE + GI_SH_UPDATES) != 0)
                {
                    float unused;
                    irradiance = giShIrradiance(b, h, e, s.normal, unused);
                    known = true;
                }
            }
            // A bounce cell without data yet (new, or not updated since): the same surface's coarser cells hold the best
            // estimate there. Irradiance 0 in its place made every young cell's first updates dark, and readers of those
            // cells (reflection hits land on fresh fine cells all the time) showed it as dark spots.
            if (!known)
            {
                float weight;
                irradiance = giCacheIrradianceAt(b, h, s.position, s.normal, bounceLevel, weight);  // coarser, then finer levels
            }
            const float3 l = normalize(g_sunDirection);
            const float cosSun = dot(s.normal, l);
            float3 sun = 0;
            if (cosSun > 0 && (P[3].w & 16) == 0)  // 16 (attribution): no sun at GI hits
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
                    sun = e0 * cosSun * (rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0);
                }
            }
            radiance = m.emissive + albedo / GI_PI * (irradiance + sun);
        }
    }

    // The raw sample for the per-ray irradiance map and SH (GiIntegrate): radiance, coordinates in the hemisphere map.
    RWStructuredBuffer<uint4> samples = ResourceDescriptorHeap[P[4].y];
    samples[thread] = uint4(asuint(radiance), (uint)round(saturate(uv.x) * 65535.0) | ((uint)round(saturate(uv.y) * 65535.0) << 16));
    const uint address = h.offTexels + (entry * GI_TEXEL_COUNT + texel) * 8;
    const uint2 old = b.Load2(address);
    const float3 previous = float3(f16tof32(old.x), f16tof32(old.x >> 16), f16tof32(old.y)) * GI_LOAD_SCALE;
    const float3 value = lerp(previous, radiance, alpha);
    const float dist = lerp(f16tof32(old.y >> 16), min(distanceToHit, 65000.0), alpha);
    const float3 stored = value * GI_STORE_SCALE;
    b.Store2(address, uint2(giPackHalf2(stored.r, stored.g), giPackHalf2(stored.b, dist)));
}
