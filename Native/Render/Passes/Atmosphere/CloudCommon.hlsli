// Volumetric clouds (B5; S_STATUS_KO.md 9): the density field of CloudModel.cpp (same formulas; the CPU reference and the
// tests use that file) and the cloud record the kernels read. Owner: S.
//
// Cloud record (raw buffer, CloudSystem.cpp CloudRecord, 240 B):
//   [0]  base altitude, top altitude, coverage, sigma_max (1/m)
//   [1]  albedo, detail strength, g0, g1
//   [2]  lobe blend, 1 / shape period, 1 / detail period, 1 / weather period
//   [3]  shape offset xyz (periods), planet bottom radius (m)
//   [4]  detail offset xyz, sky dome SRV (RGBA16F, cloudDomeUv: the layer seen from the camera in every direction)
//   [5]  weather offset xy, cloud layer SRV (RGBA16F), cloud distance SRV (R16F, km) (the frame's; readers)
//   [6]  world origin offset xyz (world = renderer space + origin), 0
//   [7]  SRVs: shape, detail (Texture3D RG8 with mips: the noise, its deviation under a mip's texel), weather (Texture2D
//        RG8), shadow (Texture2D RGBA32_UINT)
//   [8]  toward the sun xyz (unit), shadow half extent (m)
//   [9]  shadow centre xyz (renderer space), shadow texels per side
//   [10] sun illuminance at the layer (lux, rgb), sky radiance over the upper hemisphere (tests, CloudMarch mode 4)
//   [11] the cirrus sheet: altitude (m), vertical optical depth where its map is full, coverage (0: none), 1 / map period
//   [12] cirrus map offset xy (periods), cirrus map SRV (Texture2D R8 with mips), powder (atmosphere.clouds.powder)
//   [13] a lightning flash: position xyz (renderer space), the channel's radius (m)
//   [14] its luminous intensity x colour (cd, rgb; 0: none), 0
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
    uint skySrv;
    float2 weatherOffset;
    uint layerSrv, distanceSrv;
    float3 origin;
    uint shape, detail, weather, shadow;
    float3 sunDir;
    float shadowHalfExtent;
    float3 shadowCentre;
    float shadowTexels;
    float3 sunIlluminance;
    float skyRadianceTest;
    float cirrusAltitude, cirrusTau, cirrusCoverage, invCirrus;
    float2 cirrusOffset;
    uint cirrus;
    float powder;
    float3 flashPosition;
    float flashRadius;
    float3 flashIntensity;
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
    c.skySrv = asuint(q4.w);
    c.weatherOffset = q5.xy;
    c.layerSrv = asuint(q5.z), c.distanceSrv = asuint(q5.w);
    c.origin = q6.xyz;
    c.shape = q7.x, c.detail = q7.y, c.weather = q7.z, c.shadow = q7.w;
    c.sunDir = q8.xyz, c.shadowHalfExtent = q8.w;
    c.shadowCentre = q9.xyz, c.shadowTexels = q9.w;
    c.sunIlluminance = q10.xyz;
    c.skyRadianceTest = q10.w;
    const float4 q11 = asfloat(b.Load4(176)), q12 = asfloat(b.Load4(192)), q13 = asfloat(b.Load4(208));
    c.cirrusAltitude = q11.x, c.cirrusTau = q11.y, c.cirrusCoverage = q11.z, c.invCirrus = q11.w;
    c.cirrusOffset = q12.xy;
    c.cirrus = asuint(q12.z);
    c.powder = q12.w;
    c.flashPosition = q13.xyz, c.flashRadius = q13.w;
    c.flashIntensity = asfloat(b.Load3(224));
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

// The mean density over a stretch of 'footprint' metres around x (atmosphere.clouds.filtered_steps): what a march step
// longer than the noise's texels stands for. A point sample of the field at such a step's middle is one draw of a
// quantity that varies over the step: neighbouring rays draw differently (structure where the cloud is smooth) and,
// exp(-tau) being convex, the mean transmittance of such draws is above that of the mean optical depth (the sun's light
// under thick cloud came out too bright with 40 .. 320 m steps toward the sun).
// The shape and detail noise are read at the mip whose texel is the footprint; a mip texel holds the mean of the texels
// under it and, in its second channel, their standard deviation x 2 (CloudGpu.cpp uploadTextures). The density is a
// clamped function of both, so the density of the mean noise is not the mean density: the two clamps are averaged in
// closed form with the values under the footprint taken as spread evenly about their mean with that deviation (half
// width sqrt(3) x deviation) and the two noises as independent - derived, not fitted; the erosion's division is
// linearised about the means. At mip 0 the deviation is 0 and this is cloudDensity. The weather map and the height
// profile vary over kilometres and hundreds of metres: they are taken at x.
#define CLOUD_SHAPE_TEXELS 128.0   // CloudModel.h kShapeSize, kDetailSize
#define CLOUD_DETAIL_TEXELS 32.0
// Mean of saturate(v) for v spread evenly over [m - w, m + w]; spread: the half width of the even spread that has the
// result's variance.
float cloudMeanClamp(float m, float w, out float spread)
{
    spread = 0;
    if (!(w > 1e-3)) return saturate(m);
    const float lo = m - w, hi = m + w;
    if (lo >= 0 && hi <= 1)
    {
        spread = w;
        return m;
    }
    // the shares of the spread under 0 (value 0), over 1 (value 1) and between (the value itself, from l to h)
    const float under = saturate(-lo / (2 * w)), over = saturate((hi - 1) / (2 * w)), inside = max(1 - under - over, 0.0);
    const float l = saturate(lo), h = saturate(hi);
    const float mean = inside * 0.5 * (l + h) + over;
    // the variance from the nearer clamp (small numbers stay small: no difference of values near 1)
    float variance;
    if (mean <= 0.5) variance = inside * (l * l + l * h + h * h) * (1.0 / 3.0) + over - mean * mean;
    else
    {
        const float a = 1 - h, b = 1 - l, low = inside * 0.5 * (a + b) + under;
        variance = inside * (a * a + a * b + b * b) * (1.0 / 3.0) + under - low * low;
    }
    spread = sqrt(max(3 * variance, 0.0));
    return mean;
}
float cloudDensityFiltered(CloudRecord c, float3 x, float footprint)
{
    const float hn = (cloudAltitude(c, x) - c.base) / (c.top - c.base);
    if (hn <= 0 || hn >= 1 || c.coverage <= 0) return 0;
    Texture2D<float2> weather = ResourceDescriptorHeap[c.weather];
    const float2 w = weather.SampleLevel(g_linearWrap, x.xz * c.invWeather + c.weatherOffset, 0);
    const float cov = saturate(w.x * 2 * c.coverage);
    if (cov <= 0) return 0;
    const float topN = 0.35 + 0.65 * w.y;
    const float profile = saturate(hn / 0.08) * saturate((topN - hn) / (0.25 * topN));
    Texture3D<float2> shape = ResourceDescriptorHeap[c.shape];
    const float2 s = shape.SampleLevel(g_linearWrap, x * c.invShape + c.shapeOffset, log2(max(footprint * c.invShape * CLOUD_SHAPE_TEXELS, 1.0)));
    float baseSpread;
    const float base = cloudMeanClamp((s.x * profile - (1 - cov)) / cov, 0.8660254 * s.y * profile / cov, baseSpread);  // (sqrt(3) x (y / 2))
    if (base <= 0) return 0;
    Texture3D<float2> detail = ResourceDescriptorHeap[c.detail];
    const float2 d = detail.SampleLevel(g_linearWrap, x * c.invDetail + c.detailOffset, log2(max(footprint * c.invDetail * CLOUD_DETAIL_TEXELS, 1.0)));
    const float erosion = d.x * c.detailStrength, erosionSpread = 0.8660254 * d.y * c.detailStrength;
    const float inv = 1.0 / max(1 - erosion, 1e-4);
    // (base - erosion) / (1 - erosion) about the means: its slope in base is inv, in erosion (base - 1) inv^2
    const float slope = (base - 1) * inv * inv;
    float unused;
    return c.sigmaMax * cloudMeanClamp((base - erosion) * inv, sqrt(baseSpread * baseSpread * inv * inv + erosionSpread * erosionSpread * slope * slope), unused);
}

// The sky dome's parameterization (the cloud layer seen from the camera, every azimuth: clouds have no symmetry about the
// sun's plane): u = azimuth / 2 pi, v = sqrt((e - e0) / (pi/2 - e0)) with elevation e from e0 = -10 deg (dense near the
// horizon, where the clouds' angular features are smallest).
#define CLOUD_DOME_E0 (-0.17453293)
float2 cloudDomeUv(float3 d)
{
    const float azimuth = atan2(d.z, d.x);
    const float e = asin(clamp(d.y, -1.0, 1.0));
    return float2(azimuth * (0.5 / 3.14159265) + 0.5, sqrt(saturate((e - CLOUD_DOME_E0) / (1.57079633 - CLOUD_DOME_E0))));
}
float3 cloudDomeDir(float2 uv)
{
    const float azimuth = (uv.x - 0.5) * 2 * 3.14159265, e = CLOUD_DOME_E0 + uv.y * uv.y * (1.57079633 - CLOUD_DOME_E0);
    return float3(cos(e) * cos(azimuth), sin(e), cos(e) * sin(azimuth));
}

float cloudHg(float g, float cosTheta) { return (1 - g * g) / (4 * 3.14159265 * pow(max(1 + g * g - 2 * g * cosTheta, 1e-6), 1.5)); }
float cloudPhase(CloudRecord c, float cosTheta) { return (1 - c.lobeBlend) * cloudHg(c.g0, cosTheta) + c.lobeBlend * cloudHg(c.g1, cosTheta); }

// APPROXIMATION, NOT EXACT (user decision 2026-09-27 06:40; S_STATUS_KO.md 9): multiple scattering by the octave series
// and the sky's light by a height ramp, fitted to the CPU path tracer (Tests/CloudMsFit.cpp; the errors are recorded there
// and in S_STATUS). Same values as CloudModel.h kMs* / kSky*. The exact grid solve replaces it after the weekly reset.
//   sun: sum_{k < N} a^k p_k(theta) exp(-b^k tau_sun), p_k the dual-lobe HG at (g0 c^k, g1 c^k) (k = 0: single scattering)
//   sky: L_sky max(0, s0 + s1 h_n)
#define CLOUD_MS_A 0.70
#define CLOUD_MS_B 0.15
#define CLOUD_MS_C 0.60
#define CLOUD_MS_OCTAVES 2
#define CLOUD_SKY_S0 0.152
#define CLOUD_SKY_S1 1.451
struct CloudMsPhases
{
    float p[CLOUD_MS_OCTAVES];  // a^k p_k(theta): constant along a view ray
};
CloudMsPhases cloudMsPhases(CloudRecord c, float cosTheta)
{
    CloudMsPhases m;
    float ak = 1, ck = 1;
    [unroll] for (uint k = 0; k < CLOUD_MS_OCTAVES; ++k)
    {
        m.p[k] = ak * ((1 - c.lobeBlend) * cloudHg(c.g0 * ck, cosTheta) + c.lobeBlend * cloudHg(c.g1 * ck, cosTheta));
        ak *= CLOUD_MS_A, ck *= CLOUD_MS_C;
    }
    return m;
}
// powder (atmosphere.clouds.powder; 0: the fitted series): the octaves past the first - the multiple scattering - x
// (1 - powder exp(-2 tau_sun)): light scattered several times needs cloud around it, and a sample just under the sunlit
// surface has little toward the sun. The single scattering (k = 0) is exact and stays. CloudModel.h referenceApproximate
// is the twin; Tests/CloudMsFit.cpp --sweep fits the strength with a, b, c.
float cloudMsSun(CloudMsPhases m, float tauSun, float powder = 0)
{
    const float deep = 1 - powder * exp(-2 * tauSun);
    float sum = 0, bk = 1;
    [unroll] for (uint k = 0; k < CLOUD_MS_OCTAVES; ++k)
    {
        sum += m.p[k] * exp(-bk * tauSun) * (k > 0 ? deep : 1.0);
        bk *= CLOUD_MS_B;
    }
    return sum;
}
float cloudSkyRamp(CloudRecord c, float3 x) { return max(0.0, CLOUD_SKY_S0 + CLOUD_SKY_S1 * (cloudAltitude(c, x) - c.base) / (c.top - c.base)); }
// The ground's light (atmosphere.clouds.ground_light; the frame only): the lower hemisphere at the ground's radiance
// enters the layer from its base as the sky's enters from its top - the sky's ramp mirrored in height. NOT in the path
// tracer's fit (its ground is black): an approximation on top of an approximation, for cloud bases that are not lit by
// the sky alone under a high sun.
float cloudGroundRamp(CloudRecord c, float3 x) { return max(0.0, CLOUD_SKY_S0 + CLOUD_SKY_S1 * (c.top - cloudAltitude(c, x)) / (c.top - c.base)); }

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

// ---- The cirrus sheet (CloudModel.h CloudLayer::cirrus*): a second, thin layer type - ice cloud on the sphere at one
// altitude, its vertical optical depth from a map of fibres (CloudModel.cpp generateNoise: R8 with mips, period 131 km):
// tau = cirrusTau x the map's value over the coverage's threshold. Where the ray (o, d unit) meets the sheet: the
// distance t along it, the sheet's vertical optical depth there and |cosine| of the ray to the sheet's normal (not under
// 0.05). False: no sheet, the ray never meets it, or the planet is in the way. footprintPerMetre: the ray's footprint
// per metre of distance (the pixel angle) - the map is read at the mip of the footprint on the sheet.
#define CLOUD_CIRRUS_TEXELS 512.0  // CloudModel.h kCirrusSize
bool cloudCirrusAt(CloudRecord c, float3 o, float3 d, float footprintPerMetre, out float t, out float tau, out float mu)
{
    t = 0;
    tau = 0;
    mu = 1;
    if (!(c.cirrusCoverage > 0)) return false;
    const float h = cloudAltitude(c, o), R = c.bottomRadius, a = c.cirrusAltitude;
    const float3 w = o + c.origin;
    const float b = dot(w, d) + R * d.y;
    float tn, tf;
    if (!cloudSphereRoots(b, (h - a) * (2 * R + h + a), tn, tf)) return false;
    t = h < a ? tf : tn;  // under the sheet: where the ray leaves its sphere; over it: where it enters
    if (!(t > 0)) return false;
    float gn, gf;
    if (h < a && cloudSphereRoots(b, h * (2 * R + h), gn, gf) && gn > 0) return false;  // (the planet first)
    const float3 x = o + d * t;
    mu = max(abs(dot(normalize(x + c.origin + float3(0, R, 0)), d)), 0.05);
    Texture2D<float> map = ResourceDescriptorHeap[c.cirrus];
    const float lod = log2(max(footprintPerMetre * t / mu * c.invCirrus * CLOUD_CIRRUS_TEXELS, 1.0));
    const float n = map.SampleLevel(g_linearWrap, x.xz * c.invCirrus + c.cirrusOffset, lod);
    tau = c.cirrusTau * saturate((n - (1 - c.cirrusCoverage)) / max(c.cirrusCoverage, 1e-3));
    return tau > 0;
}
#endif
