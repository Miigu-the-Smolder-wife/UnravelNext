// Volumetric clouds (B5; S_STATUS_KO.md 9): the density field of CloudModel.cpp (same formulas; the CPU reference and the
// tests use that file) and the cloud record the kernels read. Owner: S.
//
// Cloud record (raw buffer, CloudSystem.cpp CloudRecord, 176 B):
//   [0]  base altitude, top altitude, coverage, sigma_max (1/m)
//   [1]  albedo, detail strength, g0, g1
//   [2]  lobe blend, 1 / shape period, 1 / detail period, 1 / weather period
//   [3]  shape offset xyz (periods), planet bottom radius (m)
//   [4]  detail offset xyz, 0
//   [5]  weather offset xy, cloud layer SRV (RGBA16F), cloud distance SRV (R16F, km) (the frame's; readers)
//   [6]  world origin offset xyz (world = renderer space + origin), 0
//   [7]  SRVs: shape (Texture3D R8), detail (Texture3D R8), weather (Texture2D RG8), shadow (Texture2D RGBA32_UINT)
//   [8]  toward the sun xyz (unit), shadow half extent (m)
//   [9]  shadow centre xyz (renderer space), shadow texels per side
//   [10] sun illuminance at the layer (lux, rgb), 0
#ifndef UNX_CLOUD_COMMON_HLSLI
#define UNX_CLOUD_COMMON_HLSLI
#include "Bindless.hlsli"

struct CloudRecord
{
    float base, top, coverage, sigmaMax;
    float albedo, detailStrength, g0, g1;
    float lobeBlend, invShape, invDetail, invWeather;
    float3 shapeOffset;
    float bottomRadius;
    float3 detailOffset;
    float2 weatherOffset;
    uint layerSrv, distanceSrv;
    float3 origin;
    uint shape, detail, weather, shadow;
    float3 sunDir;
    float shadowHalfExtent;
    float3 shadowCentre;
    float shadowTexels;
    float3 sunIlluminance;
};

CloudRecord cloudLoad(uint rawBuffer)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[rawBuffer];
    const float4 q0 = asfloat(b.Load4(0)), q1 = asfloat(b.Load4(16)), q2 = asfloat(b.Load4(32)), q3 = asfloat(b.Load4(48)), q4 = asfloat(b.Load4(64));
    const float4 q5 = asfloat(b.Load4(80)), q6 = asfloat(b.Load4(96)), q8 = asfloat(b.Load4(128)), q9 = asfloat(b.Load4(144)), q10 = asfloat(b.Load4(160));
    const uint4 q7 = b.Load4(112);
    CloudRecord c;
    c.base = q0.x, c.top = q0.y, c.coverage = q0.z, c.sigmaMax = q0.w;
    c.albedo = q1.x, c.detailStrength = q1.y, c.g0 = q1.z, c.g1 = q1.w;
    c.lobeBlend = q2.x, c.invShape = q2.y, c.invDetail = q2.z, c.invWeather = q2.w;
    c.shapeOffset = q3.xyz, c.bottomRadius = q3.w;
    c.detailOffset = q4.xyz;
    c.weatherOffset = q5.xy;
    c.layerSrv = asuint(q5.z), c.distanceSrv = asuint(q5.w);
    c.origin = q6.xyz;
    c.shape = q7.x, c.detail = q7.y, c.weather = q7.z, c.shadow = q7.w;
    c.sunDir = q8.xyz, c.shadowHalfExtent = q8.w;
    c.shadowCentre = q9.xyz, c.shadowTexels = q9.w;
    c.sunIlluminance = q10.xyz;
    return c;
}

// Altitude above the planet (centre at world (0, -R, 0)): (h^2 + y^2 + 2 y R) / (|p| + R), no cancellation in float.
float cloudAltitude(CloudRecord c, float3 x)
{
    const float3 w = x + c.origin;
    const float h2 = w.x * w.x + w.z * w.z;
    return (h2 + w.y * w.y + 2 * w.y * c.bottomRadius) / (sqrt(h2 + (w.y + c.bottomRadius) * (w.y + c.bottomRadius)) + c.bottomRadius);
}

// Density (extinction, 1/m) at renderer-space x (CloudModel.cpp density()).
float cloudDensity(CloudRecord c, float3 x)
{
    const float hn = (cloudAltitude(c, x) - c.base) / (c.top - c.base);
    if (hn <= 0 || hn >= 1 || c.coverage <= 0) return 0;
    Texture2D<float2> weather = ResourceDescriptorHeap[c.weather];
    const float2 w = weather.SampleLevel(g_linearWrap, x.xz * c.invWeather + c.weatherOffset, 0);
    const float cov = saturate(w.x * 2 * c.coverage);
    if (cov <= 0) return 0;
    const float topN = 0.35 + 0.65 * w.y;
    const float profile = saturate(hn / 0.08) * saturate((topN - hn) / (0.25 * topN));
    Texture3D<float> shape = ResourceDescriptorHeap[c.shape];
    const float base = saturate((shape.SampleLevel(g_linearWrap, x * c.invShape + c.shapeOffset, 0) * profile - (1 - cov)) / cov);
    if (base <= 0) return 0;
    Texture3D<float> detail = ResourceDescriptorHeap[c.detail];
    const float erosion = detail.SampleLevel(g_linearWrap, x * c.invDetail + c.detailOffset, 0) * c.detailStrength;
    return c.sigmaMax * saturate((base - erosion) / max(1 - erosion, 1e-4));
}

float cloudHg(float g, float cosTheta) { return (1 - g * g) / (4 * 3.14159265 * pow(max(1 + g * g - 2 * g * cosTheta, 1e-6), 1.5)); }
float cloudPhase(CloudRecord c, float cosTheta) { return (1 - c.lobeBlend) * cloudHg(c.g0, cosTheta) + c.lobeBlend * cloudHg(c.g1, cosTheta); }

// Roots of t^2 + 2 b t + C = 0 (a ray against a sphere: b = (p - centre) . d, C = |p - centre|^2 - r^2) in the stable
// form (no cancellation between -b and the square root). False when the ray misses the sphere.
bool cloudSphereRoots(float b, float C, out float tNear, out float tFar)
{
    const float disc = b * b - C;
    tNear = tFar = 0;
    if (disc <= 0) return false;
    const float q = -b - (b >= 0 ? 1.0 : -1.0) * sqrt(disc);
    const float r0 = q, r1 = q != 0 ? C / q : 0;
    tNear = min(r0, r1);
    tFar = max(r0, r1);
    return true;
}
// The first span of a ray (origin o, unit d) inside the layer's altitude shell [base, top], clipped to [0, tMax]. C for
// each sphere comes from the camera's altitude h: |p|^2 - (R + a)^2 = (h - a)(2R + h + a), exact in float; b = p . d with
// p relative to the planet's centre (0.5 m absolute error at R: far below the layer's scale).
bool cloudShellSpan(CloudRecord c, float3 o, float3 d, float tMax, out float t0, out float t1)
{
    const float h = cloudAltitude(c, o), R = c.bottomRadius;
    const float3 w = o + c.origin;
    const float b = dot(w, d) + R * d.y;
    float nT, fT, nB, fB;
    t0 = t1 = 0;
    if (!cloudSphereRoots(b, (h - c.top) * (2 * R + h + c.top), nT, fT)) return false;
    float enter = max(nT, 0.0), leave = fT;
    if (cloudSphereRoots(b, (h - c.base) * (2 * R + h + c.base), nB, fB))
    {
        if (h < c.base) enter = max(enter, fB);   // below the base: the span starts where the ray leaves the inner sphere
        else if (nB > 0) leave = min(leave, nB);  // above it, heading into it: the first span ends there
    }
    t0 = enter;
    t1 = min(leave, tMax);
    return t1 > t0;
}
#endif
