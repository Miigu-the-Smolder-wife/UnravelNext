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
// P[0] = { cache UAV, ray budget (dispatch width), hit cell footprint scale (float bits), ShadowSrvs buffer (raw; UNX_NONE =
//          no VSM: every sunlit hit traces a shadow ray) }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli: SKY0 atmosphere LUTs, SKY1 constants), ray length; P[3].w = gi.experiment_disable
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view (sun, scene buffers).
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"

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
    const uint seed = giRandom(entry * 9781u + h.frame * 6271u + texel * 26699u);
    const float2 uv = (float2(texel % GI_TEXELS, texel / GI_TEXELS) + float2(giUnit(seed), giUnit(seed + 1))) / GI_TEXELS;
    const float3 local = giHemiOctDecode(uv);
    RayDesc r;
    r.Origin = anchor + n * giBias(h, anchor);
    r.Direction = normalize(t * local.x + bt * local.y + n * local.z);
    r.TMin = 0;
    r.TMax = giRayLength();
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI);

    float3 radiance;
    float distanceToHit;
    if (hit.t < 0)
    {
        radiance = giSkyRadiance(r.Direction);
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
            const uint e = giFindOrCreate(b, h, giSurfaceKey(h, s.position, s.normal, bounceLevel), s.position, s.normal, created);
            if (e != GI_ENTRY_PENDING)
            {
                giTouch(b, h, e);
                giRequestHit(b, h, e);
                if (!created && b.Load(h.offSh + e * GI_SH_STRIDE + GI_SH_UPDATES) != 0)
                {
                    float unused;
                    irradiance = giShIrradiance(b, h, e, s.normal, unused);
                }
            }
            const float3 l = normalize(g_sunDirection);
            const float cosSun = dot(s.normal, l);
            float3 sun = 0;
            if (cosSun > 0)
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    // S's VSM where it holds the hit at the GI ray's footprint (the direct view's estimator), else a
                    // shadow ray (request 20260925_R_sun_visibility_at_hits.md).
                    bool resident = false;
                    float visibility = 0;
                    if (P[0].w != UNX_NONE && (P[3].w & 4) == 0)
                    {
                        ByteAddressBuffer vb = ResourceDescriptorHeap[P[0].w];
                        const uint4 a = vb.Load4(0), c = vb.Load4(16);
                        ShadowSrvs vsm;
                        vsm.pageTable = a.x; vsm.pool = a.y; vsm.blocks = a.z; vsm.searchBound = a.w;
                        vsm.constants = c.x; vsm.lights = c.y; vsm.pad0 = c.z; vsm.pad1 = c.w;
                        visibility = shadowSunVisibilityAt(vsm, s.position, s.geometricNormal, hit.t * GI_FOOTPRINT_PER_METRE * asfloat(P[0].z), resident);
                    }
                    if (!resident)
                    {
                        RayDesc sr;
                        sr.Origin = s.position + s.normal * giBias(h, s.position);
                        sr.Direction = giSunDirection(seed + 7);
                        sr.TMin = 0;
                        sr.TMax = giRayLength();
                        visibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                    }
                    sun = e0 * cosSun * visibility;
                }
            }
            radiance = m.emissive + albedo / GI_PI * (irradiance + sun);
        }
    }

    const uint address = h.offTexels + (entry * GI_TEXEL_COUNT + texel) * 8;
    const uint2 old = b.Load2(address);
    const float3 previous = float3(f16tof32(old.x), f16tof32(old.x >> 16), f16tof32(old.y)) * GI_LOAD_SCALE;
    const float3 value = lerp(previous, radiance, alpha);
    const float dist = lerp(f16tof32(old.y >> 16), min(distanceToHit, 65000.0), alpha);
    const float3 stored = value * GI_STORE_SCALE;
    b.Store2(address, uint2(giPackHalf2(stored.r, stored.g), giPackHalf2(stored.b, dist)));
}
