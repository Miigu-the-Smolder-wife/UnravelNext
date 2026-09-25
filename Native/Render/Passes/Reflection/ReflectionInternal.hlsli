// R-internal reflection helpers (classification, jobs, GGX sampling). Not for other tracks.
#ifndef UNX_REFLECTION_INTERNAL_HLSLI
#define UNX_REFLECTION_INTERNAL_HLSLI
#include "Passes/Reflection/Reflection.hlsli"
#include "GBuffer.hlsli"

// Per-pixel reflection mode texture (R32_UINT, written by ReflectionClassify in tiles that need rays):
//   bits 0-1 mode (REFL_K, REFL_M, REFL_G), bits 2-4 log2 of the G sample spacing, bits 8-31 job index (REFL_NO_JOB).
#define REFL_K 0u
#define REFL_M 1u
#define REFL_G 2u
#define REFL_PLANAR 3u  // planar mirror pixel: the reflection camera's colour (plane index in the spacing bits)
#define REFL_NO_JOB 0xFFFFFFu

uint reflPackMode(uint mode, uint spacingLog2, uint job) { return mode | (spacingLog2 << 2) | (job << 8); }
uint reflMode(uint v) { return v & 3u; }
uint reflSpacing(uint v) { return 1u << ((v >> 2) & 7u); }
uint reflJob(uint v) { return v >> 8; }

// Planar reflector candidates of this frame (ReflectionSystem's upload ring, raw SRV at a byte offset):
// { candidates, views, pad x 2, 64 x { float4 plane, uint4 rect (x, y, width, height) } }; candidates [0, views) have a
// reflection camera.
struct ReflPlanar
{
    float4 plane;
    uint4 rect;
};
uint2 reflPlanarCounts(uint srv, uint offset)  // candidates, views
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    return b.Load2(offset);
}
ReflPlanar reflPlanar(uint srv, uint offset, uint index)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    ReflPlanar p;
    p.plane = asfloat(b.Load4(offset + 16 + index * 32));
    p.rect = b.Load4(offset + 32 + index * 32);
    return p;
}

// A job's value from its n unmasked samples: sumL = sum of the rays' radiance, sumG = sum of the control variate g (the
// screen-probe cache in each ray's direction; 0 for M jobs), gbar = the control variate's lobe integral. The estimate is
// mean(L) + beta (gbar - mean(g)) per channel with beta = min(1, mean(L) / mean(g)): where the rays are at least as bright
// as the cache predicts it is the difference estimator gbar + mean(L - g) (beta = 1, >= gbar); where they are darker
// (the cache sees past an occluder near the surface: probe parallax, contact shadows) it is the ratio estimator
// gbar mean(L) / mean(g), never negative. The difference estimator alone went negative there and was clamped to 0: black
// G samples spread over their spacing (dark blocks and dots on glossy surfaces near contacts). Both agree at
// mean(L) = mean(g); no samples: gbar.
float3 reflLobeEstimate(float3 sumL, float3 sumG, uint n, float3 gbar)
{
    if (n == 0) return gbar;
    const float3 meanL = sumL / n, meanG = sumG / n;
    if ((P[5].x >> 24) & 128) return max(gbar + meanL - meanG, 0.0);  // attribution: the difference estimator alone
    const float3 beta = select(meanG > 1e-8, min(meanL / max(meanG, 1e-8), 1.0), 1.0);
    return max(meanL + beta * (gbar - meanG), 0.0);
}

// True when reflLobeEstimate takes the ratio branch in some channel (diagnostics).
bool reflLobeRatio(float3 sumL, float3 sumG, uint n) { return n > 0 && any(and(sumG > 1e-8 * n, sumL < sumG)); }

// A job = a pixel that traces: an M pixel (1 ray) or a G sample (4 rays). Encoded as x | y << 16.
uint reflPackPixel(uint2 p) { return p.x | (p.y << 16); }
uint2 reflUnpackPixel(uint v) { return uint2(v & 0xFFFFu, v >> 16); }

// Job result: lobe-normalised radiance (fp16 RGB, x 1/64 like the GI cache) and hit distance (fp16).
#define REFL_STORE_SCALE (1.0 / 64.0)
uint2 reflPackResult(float3 radiance, float distance)
{
    const float3 r = radiance * REFL_STORE_SCALE;
    return uint2(f32tof16(r.r) | (f32tof16(r.g) << 16), f32tof16(r.b) | (f32tof16(min(distance, 65000.0)) << 16));
}
float3 reflResultRadiance(uint2 v) { return float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) * 64.0; }
float reflResultDistance(uint2 v) { return f16tof32(v.y >> 16); }

// Surface of a pixel (main view): world position, shading normal, perceptual roughness, unit vector to the eye.
struct ReflSurface
{
    float3 position, normal, view;
    float roughness, linearDepth;
    bool valid;
};

ReflSurface reflSurface(Texture2D<float> depth, Texture2D<uint2> gbuffer, uint2 pixel)
{
    ReflSurface s;
    const float d = depth.Load(int3(pixel, 0));
    s.valid = d > 0;
    s.linearDepth = linearDepth(max(d, 1e-30));
    s.position = worldFromDepth(float2(pixel), d);
    const GBufferSample g = decodeGBuffer(gbuffer.Load(int3(pixel, 0)));
    s.normal = g.normal;
    s.roughness = g.roughness;
    s.view = normalize(g_cameraPosition - s.position);
    return s;
}

// GGX visible-normal sampling (Heitz 2018) around n for view v: a reflected direction distributed as D_v(h) mapped by
// reflection, i.e. proportional to the lobe the specular BRDF integrates (lobe-normalised average = mean of samples).
float3 reflSampleGgx(float3 n, float3 v, float alpha, float2 u)
{
    const float s = n.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    const float3 t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    const float3 b = float3(c, s + n.y * n.y * a, -n.y);
    const float3 ve = float3(dot(v, t), dot(v, b), dot(v, n));
    const float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, max(ve.z, 1e-4)));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3(-vh.y, vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x);
    const float phi = 6.28318530718 * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float sv = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - sv) * sqrt(1.0 - p1 * p1) + sv * p2;
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    const float3 he = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
    const float3 h = t * he.x + b * he.y + n * he.z;
    return reflect(-v, h);
}

#endif
