// Light functions (A8; FEATURES_GAME 12). Owner: E. Readers: M (shading), S (froxel in-scattering), R (hit shading, GI).
// See Passes/Lights/include/unx/lights/LightFunctions.h for the definitions; LightFunctions.cpp mirrors this file
// (lights::evaluate) for the CPU.
//
// Table (raw, FrameResources::lightFunctions): header 16 B (light count), a directory of one record byte offset per
// light from byte 16 (0 = no function), then 96 B records:
//   u0 profile (0 none, 1 IES, 2 cookie, 3 gobo), u1 image SRV (Texture2D<float4>, mips), u2 width, u3 height, u4 mips,
//   u5 IES horizontal count H, u6 vertical count V, u7 IES data byte offset (H horizontal angles, V vertical angles in
//   degrees, then V x H values [v * H + h]), f8 tanX, f9 tanY, f10 rotation speed, f11 rotation phase, u12 intensity
//   key count, f13 intensity period, u14 intensity keys byte offset (float2), u15 colour key count, f16 colour period,
//   u17 colour keys byte offset (float4), f18 flicker depth, f19 flicker frequency, u20 flicker octaves, u21 seed.
#ifndef UNX_LIGHT_FUNCTION_HLSLI
#define UNX_LIGHT_FUNCTION_HLSLI
#include "Bindless.hlsli"

#define LIGHT_FUNCTION_NONE 0xFFFFFFFFu
#define LF_PI 3.14159265358979f

uint lfHash(uint v)
{
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16);
}
float lfKeys1(ByteAddressBuffer b, uint count, float period, uint offset, float t)
{
    if (count == 0) return 1;
    if (period > 0) t -= period * floor(t / period);
    float2 prev = asfloat(b.Load2(offset));
    if (t <= prev.x) return prev.y;
    for (uint k = 1; k < count; ++k)
    {
        const float2 next = asfloat(b.Load2(offset + 8 * k));
        if (t <= next.x) return lerp(prev.y, next.y, (t - prev.x) / max(next.x - prev.x, 1e-20f));
        prev = next;
    }
    return prev.y;
}
float3 lfKeys3(ByteAddressBuffer b, uint count, float period, uint offset, float t)
{
    if (count == 0) return 1;
    if (period > 0) t -= period * floor(t / period);
    float4 prev = asfloat(b.Load4(offset));
    if (t <= prev.x) return prev.yzw;
    for (uint k = 1; k < count; ++k)
    {
        const float4 next = asfloat(b.Load4(offset + 16 * k));
        if (t <= next.x) return lerp(prev.yzw, next.yzw, (t - prev.x) / max(next.x - prev.x, 1e-20f));
        prev = next;
    }
    return prev.yzw;
}
// Value noise in [-1, 1] at x >= 0 (smoothstep between hashed integer steps).
float lfNoise(uint seed, float x)
{
    const float i = floor(x), f = x - i;
    const uint k = (uint)i;
    const float a = lfHash(seed ^ lfHash(k)) * (2.0f / 4294967295.0f) - 1, b = lfHash(seed ^ lfHash(k + 1)) * (2.0f / 4294967295.0f) - 1;
    const float s = f * f * (3 - 2 * f);
    return lerp(a, b, s);
}
float lfFlicker(float depth, float frequency, uint octaves, uint seed, float t)
{
    if (!(depth > 0) || octaves == 0) return 1;
    float sum = 0, weight = 0, amplitude = 1, x = frequency * t;
    for (uint o = 0; o < octaves; ++o)
    {
        sum += amplitude * lfNoise(lfHash(seed + o * 0x9E3779B9u), x);
        weight += amplitude;
        amplitude *= 0.5f;
        x *= 2;
    }
    return max(0.0f, 1 + depth * sum / weight);
}
// Bracket of a in an ascending list at byte offset `start` (count entries): index of the lower end and the fraction.
float2 lfInterval(ByteAddressBuffer b, uint start, uint count, float a)
{
    if (count == 1) return 0;
    uint lo = 0, hi = count - 1;
    [loop] while (hi - lo > 1)
    {
        const uint mid = (lo + hi) / 2;
        if (asfloat(b.Load(start + 4 * mid)) <= a) lo = mid;
        else hi = mid;
    }
    const float x0 = asfloat(b.Load(start + 4 * lo)), x1 = asfloat(b.Load(start + 4 * hi));
    return float2(lo, saturate((a - x0) / max(x1 - x0, 1e-20f)));
}
float lfIes(ByteAddressBuffer b, uint H, uint V, uint offset, float phiDeg, float thetaDeg)
{
    const uint hAngles = offset, vAngles = offset + 4 * H, values = offset + 4 * (H + V);
    const float first = asfloat(b.Load(hAngles)), last = asfloat(b.Load(hAngles + 4 * (H - 1)));
    if (last <= 0) phiDeg = 0;                                                                        // rotationally symmetric
    else if (first >= 90) phiDeg = phiDeg < 90 ? 180 - phiDeg : (phiDeg > 270 ? 540 - phiDeg : phiDeg);  // about the 90-270 plane
    else if (last <= 90) { phiDeg = fmod(phiDeg, 180.0f); if (phiDeg > 90) phiDeg = 180 - phiDeg; }  // quadrant
    else if (last <= 180) { if (phiDeg > 180) phiDeg = 360 - phiDeg; }                              // bilateral
    const float v0 = asfloat(b.Load(vAngles)), v1 = asfloat(b.Load(vAngles + 4 * (V - 1)));
    if (thetaDeg < v0 || thetaDeg > v1) return 0;
    const float2 h = lfInterval(b, hAngles, H, phiDeg), v = lfInterval(b, vAngles, V, thetaDeg);
    const uint h0 = (uint)h.x, h1 = min(h0 + 1, H - 1), r0 = (uint)v.x, r1 = min(r0 + 1, V - 1);
    const float a = asfloat(b.Load(values + 4 * (r0 * H + h0))), c = asfloat(b.Load(values + 4 * (r0 * H + h1)));
    const float d = asfloat(b.Load(values + 4 * (r1 * H + h0))), e = asfloat(b.Load(values + 4 * (r1 * H + h1)));
    return lerp(lerp(a, c, h.y), lerp(d, e, h.y), v.y);
}

// f(direction, time): the multiplier of light `light`'s emission towards unit direction `dir` (from the light).
// forward / right: the light's axes (GpuLight). footprint: the receiver's footprint angle seen from the light (radians;
// 0 = sharpest level). table = LIGHT_FUNCTION_NONE: 1.
float3 lightFunction(uint table, uint light, float3 forward, float3 right, float3 dir, float footprint, float time)
{
    if (table == LIGHT_FUNCTION_NONE) return 1;
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    if (light >= b.Load(0)) return 1;
    const uint o = b.Load(16 + 4 * light);
    if (o == 0) return 1;
    const uint4 w0 = b.Load4(o), w1 = b.Load4(o + 16), w3 = b.Load4(o + 48), w4 = b.Load4(o + 64), w5 = b.Load4(o + 80);
    const float4 f2 = asfloat(b.Load4(o + 32));
    float3 f = 1;
    const uint profile = w0.x;
    if (profile != 0)
    {
        const float3 z = normalize(forward);
        const float3 x = normalize(right - z * dot(right, z));
        const float3 y = cross(z, x);
        const float angle = f2.w + f2.z * time;
        float sa, ca;
        sincos(angle, sa, ca);
        const float dx = dot(dir, x), dy = dot(dir, y), dz = dot(dir, z);
        const float rx = dx * ca + dy * sa, ry = -dx * sa + dy * ca;  // the direction in the profile's turned frame
        if (profile == 1)
        {
            float phi = atan2(ry, rx) * (180 / LF_PI);
            if (phi < 0) phi += 360;
            const float theta = acos(clamp(dz, -1.0f, 1.0f)) * (180 / LF_PI);
            f = lfIes(b, w1.y, w1.z, w1.w, phi, theta);
        }
        else
        {
            Texture2D<float4> image = ResourceDescriptorHeap[w0.y];
            const float width = (float)w0.z, height = (float)w0.w, mips = (float)w1.x;
            float2 uv;
            float texelAngle;
            if (profile == 2)
            {
                if (!(dz > 0)) return 0;
                const float u = rx / (dz * f2.x), v = ry / (dz * f2.y);
                if (abs(u) > 1 || abs(v) > 1) return 0;
                uv = float2(0.5f + 0.5f * u, 0.5f - 0.5f * v);
                texelAngle = 2 * f2.x / width;
            }
            else
            {
                float phi = atan2(ry, rx);
                if (phi < 0) phi += 2 * LF_PI;
                const float theta = acos(clamp(dz, -1.0f, 1.0f));
                uv = float2(phi / (2 * LF_PI), clamp(theta / LF_PI, 0.5f / height, 1 - 0.5f / height));
                texelAngle = 2 * LF_PI / width;
            }
            const float lod = footprint > 0 ? clamp(log2(footprint / texelAngle), 0.0f, mips - 1) : 0.0f;
            if (profile == 2) f = image.SampleLevel(g_linearClamp, uv, lod).rgb;
            else f = image.SampleLevel(g_linearWrap, uv, lod).rgb;
        }
    }
    f *= lfKeys1(b, w3.x, asfloat(w3.y), w3.z, time);
    f *= lfKeys3(b, w3.w, asfloat(w4.x), w4.y, time);
    f *= lfFlicker(asfloat(w4.z), asfloat(w4.w), w5.x, w5.y, time);
    return f;
}
#endif
