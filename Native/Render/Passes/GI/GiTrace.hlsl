// unx-kernel: lib_6_6 main
// unx-variants: SKY=1
// World radiance cache update rays (ARCHITECTURE 2.5; DispatchRays with alpha any-hit, 1.3-5). The fixed per-frame ray
// budget is spent as whole-hemisphere updates: 64 threads per updated entry (GiSelect's stalest-first selection, then
// background entries), thread = texel, direction jittered inside the texel. The hit's outgoing radiance =
// emission + diffuse albedo / pi * (irradiance of the hit's cell + direct sun with that cell's cached sun visibility);
// the hit cell is at least the ray's footprint (texel cone ~0.36 t) coarse and is requested for update next frame.
// Texels blend with the entry's history weight (GiInternal giHistoryAlpha). Texel 0 also samples the entry's own sun
// visibility (one shadow ray within the solar disk).
//
// P[0] = { cache UAV, ray budget (dispatch width), 0, 0 }
// P[1] = { constant sky radiance rgb (SKY1), ray length }
// P[2] = { atmosphere: transmittance, multiScatter, skyView, aerial SRVs (SKY0) }
// P[3] = { constant sun illuminance rgb (SKY1, lux), 0 }
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view (sun, scene buffers).
#define SKY_ATMOSPHERE 0  // variant .SKY0: the frame's sky (S's atmosphere LUTs); added to the variant list when S commits Atmosphere.hlsli
#define SKY_CONSTANT 1    // variant .SKY1: constant sky radiance and sun illuminance from root constants (tests)
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#if SKY == SKY_ATMOSPHERE
#include "Passes/Atmosphere/Atmosphere.hlsli"
#endif

// Texel cone of an 8 x 8 hemispherical texel (2 pi / 64 sr ~ 10.1 deg half-angle): footprint diameter ~0.36 t.
#define GI_FOOTPRINT_PER_METRE 0.36

uint giRandom(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float giUnit(uint x) { return (giRandom(x) >> 8) * (1.0 / 16777216.0); }

float3 giSkyRadiance(float3 dir)
{
#if SKY == SKY_ATMOSPHERE
    AtmosphereSrvs a = { P[2].x, P[2].y, P[2].z, P[2].w };
    return P[2].z == UNX_NONE ? 0 : atmosphereSkyRadiance(a, dir);
#else
    return asfloat(P[1].xyz);
#endif
}

// Direct solar illuminance on a surface facing the sun at p (without shadowing).
float3 giSunIlluminance(float3 p)
{
#if SKY == SKY_ATMOSPHERE
    AtmosphereSrvs a = { P[2].x, P[2].y, P[2].z, P[2].w };
    return P[2].x == UNX_NONE ? 0 : atmosphereSunIlluminance(a, p);
#else
    return asfloat(P[3].xyz);
#endif
}

// Uniform direction within the solar disk.
float3 giSunDirection(uint seed)
{
    const float3 l = normalize(g_sunDirection);
    float3 t, b;
    giBasis(l, t, b);
    const float r = tan(g_sunAngularRadius) * sqrt(giUnit(seed));
    const float phi = 6.28318530718 * giUnit(seed ^ 0x9e3779b9u);
    return normalize(l + (t * cos(phi) + b * sin(phi)) * r);
}

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
    r.TMax = asfloat(P[1].w);
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
        const GpuMaterial m = loadMaterial(s.material);
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
        if (!s.frontFace && !twoSided)
        {
            radiance = 0;  // inside closed geometry
        }
        else
        {
            const float3 albedo = m.baseColor * (1 - m.metallic);
            float3 irradiance = 0;
            float sunVisibility = 0;
            bool sunKnown = false;
            bool created;
            const uint footprintLevel = giLevelForSize(h, hit.t * GI_FOOTPRINT_PER_METRE);
            const uint e = giFindOrCreate(b, h, giSurfaceKey(h, s.position, s.normal, footprintLevel), s.position, s.normal, created);
            if (e != GI_ENTRY_PENDING)
            {
                giTouch(b, h, e);
                giRequestHit(b, h, e);
                if (!created)
                {
                    irradiance = giShIrradiance(b, h, e, s.normal, sunVisibility);
                    sunKnown = b.Load(h.offSh + e * GI_SH_STRIDE + GI_SH_SUN_SAMPLES) != 0;
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
                    if (!sunKnown)
                    {
                        RayDesc sr;
                        sr.Origin = s.position + s.normal * giBias(h, s.position);
                        sr.Direction = giSunDirection(seed + 7);
                        sr.TMin = 0;
                        sr.TMax = asfloat(P[1].w);
                        sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                    }
                    sun = e0 * cosSun * sunVisibility;
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

    // The entry's own sun visibility: one shadow ray within the solar disk per update, averaged like the texels.
    if (texel == 0)
    {
        const float3 l = normalize(g_sunDirection);
        if (dot(n, l) > 0 && any(giSunIlluminance(anchor) > 0))
        {
            RayDesc sr;
            sr.Origin = r.Origin;
            sr.Direction = giSunDirection(seed + 3);
            sr.TMin = 0;
            sr.TMax = asfloat(P[1].w);
            const float sample = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
            const uint word13 = b.Load(shAddress + 52);
            const uint samples = history == 0 ? 0 : b.Load(shAddress + GI_SH_SUN_SAMPLES);
            const float a = max(1.0 / (float)(samples + 1), 1.0 / (float)h.historyMax);
            const float visibility = lerp(f16tof32(word13 >> 16), sample, a);
            b.Store(shAddress + 52, (word13 & 0xFFFFu) | (f32tof16(visibility) << 16));
            b.Store(shAddress + GI_SH_SUN_SAMPLES, samples + 1);
        }
    }
}
