// Wind windAt(x, t) (B6, FEATURES_GAME 10; Docs/Design/Requests/20260926_B_weather_fields.md): one formula for every
// consumer, included by HLSL and by C++ (with unx::float3 and <cmath>, <cstring> in scope; the portability block below
// maps the intrinsics). The authority is the World's field records (NW_FieldRecord, quantity NW_FIELD_WIND,
// RuntimeCommon/FieldSampling.h): records in the World's evaluation order (ascending priority, then WorldKey), each with
// a domain (global, unit sphere, unit box through its inverse basis) and an operation (ADD, REPLACE, MIN, MAX per
// component; MIN / MAX start from the first contributing value), plus an optional turbulence term per record:
//   curl noise v_t = amplitude x curl psi(x / L, t / T) / rms, psi = three gradient-noise potentials (analytic
//   derivatives, 1-4 octaves of lacunarity 2 with Kolmogorov scaling: the velocity of each octave 2^(-1/3) of the one
//   before, scrolled one noise period per T in distinct directions): divergence free, so particles carried by it neither
//   gather nor thin out; amplitude = the RMS turbulent speed (m/s), L = the largest eddy (m), T = its turnover time (s).
// Positions are relative to the frame's reference point in float (the host subtracts it in double first, as the World's
// sampler subtracts the origin in double: large-world precision). The turbulence is added to the record's value before
// its operation; its noise is evaluated at the record-local offset x - origin (the same on every frame reference, and the
// World sampler's double (position - origin) in float).
#ifndef UNX_ATMOSPHERE_WIND_FIELD_HLSLI
#define UNX_ATMOSPHERE_WIND_FIELD_HLSLI

#ifdef __cplusplus
namespace unx::render::wind
{
using uint = uint32_t;
struct WindFloat4
{
    float x, y, z, w;
};
#define WF_FN inline
#define WF_OUT3 float3&
#define WF_RECORDS const WindRecord*
#define WF_CONST constexpr
inline float3 wf3(float a, float b, float c) { return { a, b, c }; }
inline float3 wfFloor3(float3 v) { return { std::floor(v.x), std::floor(v.y), std::floor(v.z) }; }
inline float wfAbs(float x) { return std::fabs(x); }
inline float wfMin(float a, float b) { return a < b ? a : b; }
inline float wfMax(float a, float b) { return a > b ? a : b; }
inline uint wfAsUint(float f)
{
    uint u;
    std::memcpy(&u, &f, 4);
    return u;
}
#else
#define WindFloat4 float4
#define WF_FN
#define WF_OUT3 out float3
#define WF_RECORDS StructuredBuffer<WindRecord>
#define WF_CONST static const
float3 wf3(float a, float b, float c) { return float3(a, b, c); }
float3 wfFloor3(float3 v) { return floor(v); }
float wfAbs(float x) { return abs(x); }
float wfMin(float a, float b) { return min(a, b); }
float wfMax(float a, float b) { return max(a, b); }
uint wfAsUint(float f) { return asuint(f); }
#endif

WF_CONST uint kWindAdd = 0, kWindReplace = 1, kWindMinimum = 2, kWindMaximum = 3;  // NW_FIELD_ADD ..
WF_CONST uint kWindGlobal = 0, kWindSphere = 1, kWindBox = 2;                       // NW_FIELD_GLOBAL ..

// GPU record, 80 B: the origin with op | shape << 8, the rows of the inverse basis with the value's components in their
// w, turbulence { amplitude m/s, length scale m, time scale s, octaves | seed << 8 (uint bits) }.
struct WindRecord
{
    float3 origin;
    uint opShape;
    WindFloat4 row0, row1, row2;
    WindFloat4 turbulence;
};

// Integer hash (PCG-style), identical in HLSL and C++ (32-bit unsigned arithmetic).
WF_FN uint wfHash(uint x)
{
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}
// Gradient of lattice point (i, j, k) for potential 'seed': one of the 12 cube-edge directions (Perlin 2002), length 1.
WF_FN float3 wfGradient(int i, int j, int k, uint seed)
{
    const uint h = wfHash(uint(i) * 73856093u ^ uint(j) * 19349663u ^ uint(k) * 83492791u ^ seed * 2654435761u);
    const uint e = h % 12u;
    const float a = (e & 1u) != 0u ? -0.70710678f : 0.70710678f, b = (e & 2u) != 0u ? -0.70710678f : 0.70710678f;
    if (e < 4u) return wf3(a, b, 0);
    if (e < 8u) return wf3(a, 0, b);
    return wf3(0, a, b);
}
// Gradient noise with its analytic gradient (quintic fade).
WF_FN float wfNoise(float3 p, uint seed, WF_OUT3 grad)
{
    const float3 pf = wfFloor3(p);
    const int i = int(pf.x), j = int(pf.y), k = int(pf.z);
    const float3 f = p - pf;
    const float3 u = wf3(f.x * f.x * f.x * (f.x * (f.x * 6 - 15) + 10), f.y * f.y * f.y * (f.y * (f.y * 6 - 15) + 10), f.z * f.z * f.z * (f.z * (f.z * 6 - 15) + 10));
    const float3 du = wf3(30 * f.x * f.x * (f.x * (f.x - 2) + 1), 30 * f.y * f.y * (f.y * (f.y - 2) + 1), 30 * f.z * f.z * (f.z * (f.z - 2) + 1));
    float value = 0;
    float3 dv = wf3(0, 0, 0);
    for (int c = 0; c < 8; ++c)
    {
        const int ox = c & 1, oy = (c >> 1) & 1, oz = c >> 2;
        const float3 g = wfGradient(i + ox, j + oy, k + oz, seed);
        const float3 d = f - wf3((float)ox, (float)oy, (float)oz);
        const float n = dot(g, d);
        const float wx = ox != 0 ? u.x : 1 - u.x, wy = oy != 0 ? u.y : 1 - u.y, wz = oz != 0 ? u.z : 1 - u.z;
        const float sx = ox != 0 ? 1.0f : -1.0f, sy = oy != 0 ? 1.0f : -1.0f, sz = oz != 0 ? 1.0f : -1.0f;
        value += wx * wy * wz * n;
        dv = dv + g * (wx * wy * wz) + wf3(sx * du.x * wy * wz, sy * du.y * wx * wz, sz * du.z * wx * wy) * n;
    }
    grad = dv;
    return value;
}
// Curl noise at p (in noise periods) and time phase tau (in time-scale units): the curl of three potentials.
WF_FN float3 wfCurl(float3 p, float tau, uint octaves, uint seed)
{
    float3 gx = wf3(0, 0, 0), gy = wf3(0, 0, 0), gz = wf3(0, 0, 0);
    float scale = 1, gain = 1;
    for (uint o = 0; o < octaves; ++o)
    {
        float3 a, b, c;
        const float fo = (float)o;
        wfNoise(p * scale + wf3(tau, 0.37f * tau, 0) + wf3(17.1f, 3.3f, 9.7f) * fo, seed * 3u + 0u, a);
        wfNoise(p * scale + wf3(0, tau, 0.61f * tau) + wf3(5.9f, 23.4f, 1.2f) * fo, seed * 3u + 1u, b);
        wfNoise(p * scale + wf3(0.53f * tau, 0, tau) + wf3(11.3f, 7.7f, 31.6f) * fo, seed * 3u + 2u, c);
        gx = gx + a * (gain * scale);
        gy = gy + b * (gain * scale);
        gz = gz + c * (gain * scale);
        scale *= 2;
        gain *= 0.39685f;  // Kolmogorov: velocity per octave x 2^(-1/3) (potential gain 2^(-4/3))
    }
    // Unit RMS speed: the measured RMS of this sum for 1-4 octaves (WindFieldTests, 200,000 points) [measured], so the
    // record's amplitude is the RMS turbulent speed. The velocity is per noise unit: the caller scales positions by 1 / L.
    const float rms = octaves <= 1u ? 1.22080f : (octaves == 2u ? 1.55885f : (octaves == 3u ? 1.74036f : 1.84400f));
    return wf3(gz.y - gy.z, gx.z - gz.x, gy.x - gx.y) * (1 / rms);
}

// The wind (m/s) at x (relative to the frame's reference point, as the records' origins) at time t (seconds).
WF_FN float3 windAt(WF_RECORDS records, uint count, float3 x, float t)
{
    float3 v = wf3(0, 0, 0);
    bool any = false;
    for (uint r = 0; r < count; ++r)
    {
        const WindRecord w = records[r];
        const uint op = w.opShape & 0xFFu, shape = (w.opShape >> 8) & 0xFFu;
        const float3 d = x - w.origin;
        const float3 l = wf3(w.row0.x * d.x + w.row0.y * d.y + w.row0.z * d.z, w.row1.x * d.x + w.row1.y * d.y + w.row1.z * d.z,
                             w.row2.x * d.x + w.row2.y * d.y + w.row2.z * d.z);
        bool inside = shape == kWindGlobal || (wfAbs(l.x) <= 1 && wfAbs(l.y) <= 1 && wfAbs(l.z) <= 1);
        if (inside && shape == kWindSphere) inside = dot(l, l) <= 1;
        if (!inside) continue;
        float3 value = wf3(w.row0.w, w.row1.w, w.row2.w);
        if (w.turbulence.x > 0)
        {
            const uint bits = wfAsUint(w.turbulence.w);
            uint octaves = bits & 0xFFu;
            if (octaves < 1u) octaves = 1u;
            if (octaves > 4u) octaves = 4u;
            // In the record's own coordinates (d = x - origin): independent of the frame's reference point (moving it
            // does not shift the pattern) and the World sampler's double (position - origin) gives the same float input.
            value = value + wfCurl(d * (1 / w.turbulence.y), t / w.turbulence.z, octaves, bits >> 8) * w.turbulence.x;
        }
        if (op == kWindAdd) v = v + value;
        else if (op == kWindReplace) v = value;
        else if (op == kWindMinimum) v = any ? wf3(wfMin(v.x, value.x), wfMin(v.y, value.y), wfMin(v.z, value.z)) : value;
        else v = any ? wf3(wfMax(v.x, value.x), wfMax(v.y, value.y), wfMax(v.z, value.z)) : value;
        any = true;
    }
    return v;
}

#ifdef __cplusplus
} // namespace unx::render::wind
#endif
#endif
