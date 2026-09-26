// Cloud sun transmittance map (CloudShadow.hlsl) layout and lookup: per texel the altitudes where the optical depth
// from the layer's top reaches CLOUD_SHADOW_LEVELS levels. Owner: S.
#ifndef UNX_CLOUD_SHADOW_COMMON_HLSLI
#define UNX_CLOUD_SHADOW_COMMON_HLSLI
#include "Passes/Atmosphere/CloudCommon.hlsli"

#define CLOUD_SHADOW_LEVELS 15u     // optical-depth levels; slot 15 holds the whole layer's optical depth (fp16): 2 RGBA32_UINT texels
#define CLOUD_SHADOW_STEP 20.0      // metres per march step of a texel's ray (the CPU reference's sun step)
#define CLOUD_SHADOW_MAX_STEPS 512u // structural bound per texel (a 2.5 km layer at a 5 deg sun: 1,435 m ... bounded)

// Optical depth of level k: dense where transmittance changes most (T from 0.95 down to 3e-4).
float cloudShadowLevel(uint k)
{
    static const float levels[15] = { 0.05, 0.1, 0.2, 0.3, 0.45, 0.65, 0.9, 1.2, 1.6, 2.1, 2.8, 3.6, 4.6, 6.0, 8.0 };
    return levels[k];
}
// unorm16 altitude over [base, top]; 0 = the ray never reaches the level (a reached one is stored as at least 1).
uint cloudShadowPackAltitude(CloudRecord c, float a, bool reached)
{
    return reached ? max(1u, (uint)round(saturate((a - c.base) / (c.top - c.base)) * 65535)) : 0u;
}
float cloudShadowUnpackAltitude(CloudRecord c, uint v) { return c.base + (v & 0xFFFFu) / 65535.0 * (c.top - c.base); }
#define CLOUD_SHADOW_RAY_BACK 50000.0  // the texel rays start this far toward the sun from the plane (above any layer)

// Orthonormal basis of the plane perpendicular to the sun direction (Duff et al. 2017).
void cloudShadowBasis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// Distance along a descending ray (origin o above the layer, unit d) to altitude a: the near root of the sphere R + a
// (cloudSphereRoots with C from the origin's altitude, exact in float).
float cloudShadowAltitudeDistance(CloudRecord c, float3 o, float3 d, float a)
{
    const float h = cloudAltitude(c, o), R = c.bottomRadius;
    const float b = dot(o + c.origin, d) + R * d.y;
    float tn, tf;
    if (!cloudSphereRoots(b, (h - a) * (2 * R + h + a), tn, tf)) return 3.0e38;  // never reaches that altitude
    return tn > 0 ? tn : tf;
}

// Optical depth from the layer's top to x along the sun: per map texel, the level altitudes bracket x's altitude and tau
// is linear in altitude between them (0 at the top); bilinear across the 4 texels around x. x outside the map: 0.
// Past the last level reached, tau runs linearly to the whole layer's optical depth at the base.
float cloudShadowTexelTau(CloudRecord c, Texture2D<uint4> map, int2 t, float a)
{
    const uint4 q0 = map[uint2(t.x * 2, t.y)], q1 = map[uint2(t.x * 2 + 1, t.y)];
    const float total = f16tof32(q1.w >> 16);
    float prevA = c.top, prevTau = 0;
    [loop] for (uint k = 0; k < CLOUD_SHADOW_LEVELS; ++k)
    {
        const uint4 q = k < 8 ? q0 : q1;
        const uint w = q[(k % 8) / 2], v = ((k & 1) ? w >> 16 : w) & 0xFFFFu;
        if (v == 0) break;  // not reached: the rest of the ray is below the last level
        const float ak = cloudShadowUnpackAltitude(c, v), tk = cloudShadowLevel(k);
        if (a >= ak) return prevA > ak ? lerp(tk, prevTau, saturate((a - ak) / (prevA - ak))) : tk;
        prevA = ak, prevTau = tk;
    }
    return prevA > c.base ? lerp(total, prevTau, saturate((a - c.base) / (prevA - c.base))) : total;
}
float cloudSunTau(CloudRecord c, float3 x)
{
    Texture2D<uint4> map = ResourceDescriptorHeap[c.shadow];
    float3 U, V;
    cloudShadowBasis(c.sunDir, U, V);
    const float3 r = x - c.shadowCentre;
    const float2 uv = float2(dot(r, U), dot(r, V)) / c.shadowHalfExtent;  // [-1, 1]
    const float n = c.shadowTexels;
    const float2 p = (uv * 0.5 + 0.5) * n - 0.5;
    if (any(p < -0.5) || any(p > n - 0.5)) return 0;
    const float a = cloudAltitude(c, x);
    const int2 i0 = int2(floor(p));
    const float2 f = p - floor(p);
    float tau = 0;
    [loop] for (uint q = 0; q < 4; ++q)
    {
        const int2 o = int2(q & 1, q >> 1);
        tau += (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * cloudShadowTexelTau(c, map, clamp(i0 + o, 0, int(n) - 1), a);
    }
    return tau;
}
// The sun's optical depth at a sample in the layer (single scattering): integrated along the sun ray in
// CLOUD_SUN_STEP midpoint steps until it leaves the layer or tau > CLOUD_SUN_TAU_MAX (T < 1.2e-4: the sample's
// contribution is below every error considered); past CLOUD_SUN_MAX_STEPS (the structural bound: long paths of a low
// sun) the map gives the rest and the value is returned negated (the caller takes its magnitude and counts the event). A map alone cannot reach this accuracy at a useful size: bilinear filtering between texel
// rays that pass through different cloud columns blends a core with open air at every edge, an error that falls only
// with the texel [measured, CloudTests WARP: 16 / 7.5 / 3.8 / 2.9 % mean at 94 / 47 / 23 / 12 m texels; near field
// exact + map 8.8 % at 47 m; this function 1.0 %].
#define CLOUD_SUN_STEP 20.0        // the near field's step (the CPU reference's)
#define CLOUD_SUN_NEAR_STEPS 4u     // steps at CLOUD_SUN_STEP before the steps grow
#ifndef CLOUD_SUN_GROWTH
#define CLOUD_SUN_GROWTH 0          // 1: past the near field the step doubles every second step, up to CLOUD_SUN_STEP_MAX
#endif
#define CLOUD_SUN_STEP_MAX 320.0
#define CLOUD_SUN_MAX_STEPS 256u
#define CLOUD_SUN_TAU_MAX 9.0
float cloudSunTauMarch(CloudRecord c, float3 x)
{
    float tau = 0, t = 0, dt = CLOUD_SUN_STEP;
    uint k = 0;
    [loop] for (; k < CLOUD_SUN_MAX_STEPS && tau < CLOUD_SUN_TAU_MAX; ++k)
    {
#if CLOUD_SUN_GROWTH
        if (k >= CLOUD_SUN_NEAR_STEPS && ((k - CLOUD_SUN_NEAR_STEPS) & 1) == 0) dt = min(dt * 2, CLOUD_SUN_STEP_MAX);
#endif
        const float3 y = x + c.sunDir * (t + 0.5 * dt);
        const float a = cloudAltitude(c, y);
        if (a > c.top || a < c.base - 1) return tau;
        tau += cloudDensity(c, y) * dt;
        t += dt;
    }
    if (tau >= CLOUD_SUN_TAU_MAX) return tau;
    // Past the structural bound: the map gives the rest; the result is returned negative so the caller counts it.
    return -(tau + cloudSunTau(c, x + c.sunDir * t));
}
#endif
