// R-internal reflection helpers (classification, jobs, GGX sampling). Not for other tracks.
#ifndef UNX_REFLECTION_INTERNAL_HLSLI
#define UNX_REFLECTION_INTERNAL_HLSLI
#include "Passes/Reflection/Reflection.hlsli"
#include "GBuffer.hlsli"

// Per-pixel reflection mode texture (R32_UINT, written by ReflectionClassify in tiles that need rays):
//   bits 0-1 mode (REFL_K, REFL_M, REFL_G), bits 2-4 log2 of the G sample spacing, bit 5 REFL_SELF (a G pixel with its
//   own job: its grid samples all disagreed last frame), bits 8-31 job index (REFL_NO_JOB).
#define REFL_K 0u
#define REFL_M 1u
#define REFL_G 2u
#define REFL_PLANAR 3u  // planar mirror pixel: the reflection camera's colour (plane index in the spacing bits)
#define REFL_NO_JOB 0xFFFFFFu
#define REFL_SELF (1u << 5)
// view.reflection and the time integration's history are RGBA16F: radiance is stored at most at the format's largest value
// (65504 nits: a glint - floor -> glossy surface -> sun - that bright is white at any daytime exposure); above it the
// store would be +inf, and M's composite would turn inf x 0 into NaN [measured: D0 720p, 3 pixels].
float3 reflStorable(float3 radiance) { return min(radiance, 65504.0); }

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
// screen-probe cache in each ray's direction; 0 for M jobs), gbar = the control variate's lobe integral (the mean of g over
// the lobe, reflLobeControl). Per channel the difference estimator gbar + mean(L - g) (unbiased) wherever it is not
// negative; where it is (the cache sees past an occluder near the surface or lags a moving object: g far brighter than
// L), the ratio estimator gbar mean(L) / mean(g), which is never negative (consistent, biased O(1/n) in that region only).
// The difference estimator alone was clamped to 0 there: black G samples spread over their spacing (the dark blocks and
// dots on glossy surfaces near contacts). With 4 samples mean(L) < mean(g) half the time where the cache is right, so
// the choice is by the sign of the difference estimate, not by mean(L) < mean(g). No samples: gbar.
float3 reflLobeEstimate(float3 sumL, float3 sumG, uint n, float3 gbar)
{
    if (n == 0) return gbar;
    const float3 meanL = sumL / n, meanG = sumG / n;
    const float3 difference = gbar + meanL - meanG;
    if ((P[5].x >> 24) & 128) return max(difference, 0.0);  // attribution: the difference estimator alone (clamped)
    return select(difference >= 0, difference, gbar * meanL / max(meanG, 1e-8));  // difference < 0 implies meanG > gbar >= 0
}

// True when reflLobeEstimate takes the ratio branch in some channel (diagnostics).
bool reflLobeRatio(float3 sumL, float3 sumG, uint n, float3 gbar) { return n > 0 && any(gbar + (sumL - sumG) / n < 0); }

// A job = a pixel that traces: an M pixel (1 ray) or a G sample (4 rays). Encoded as x | y << 16.
uint reflPackPixel(uint2 p) { return p.x | (p.y << 16); }
// (bit 31: the job has its value already - a screen trace's, ReflectionScreenTrace.hlsl - and traces no world ray)
#define REFL_JOB_DONE 0x80000000u
uint2 reflUnpackPixel(uint v) { return uint2(v & 0xFFFFu, (v >> 16) & 0x7FFFu); }

// Job result: lobe-normalised radiance (fp16 RGB, x 1/64 like the GI cache) and hit distance (fp16).
#define REFL_STORE_SCALE (1.0 / 64.0)
// A job's result (12 B): lobe radiance (fp16 x 3), mean hit distance (fp16) and the largest motion of its rays' hits:
// the hit point's displacement since the previous tick over the ray's footprint there (reflHitMotion; 0 = static).
// ReflectionAccumulate limits the value's time window by it.
uint3 reflPackResult(float3 radiance, float distance, float motion)
{
    const float3 r = radiance * REFL_STORE_SCALE;
    return uint3(f32tof16(r.r) | (f32tof16(r.g) << 16), f32tof16(r.b) | (f32tof16(min(distance, 65000.0)) << 16), asuint(motion));
}
float3 reflResultRadiance(uint3 v) { return float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) * 64.0; }
float reflResultDistance(uint3 v) { return f16tof32(v.y >> 16); }
float reflResultMotion(uint3 v) { return asfloat(v.z); }

// ---- Reconstruction layers (RENDERER_REDESIGN_V2 1.2, P2; reflection.layers). A reflection value is kept as
//     value = base + residual + albedo x stochastic,
//   stochastic  L_rs: the hits' stochastic light (HitShading.hlsli rtHitRadianceSplit) over 'albedo', the hits'
//               directional reflectance - the part a filter guided by the hit geometry reconstructs every frame;
//   residual    L_g (band-limited by the lobe's footprint blur_px); 0 for M. Two definitions, chosen where the pixels'
//               layers are written (ReflectionResolve, reflection.layer_residual_whole):
//               whole (true)       a G pixel's lobe estimate without the stochastic share: the control variate's lobe
//                                  integral plus the rays' deterministic difference to it; the base is 0 for G;
//               difference (false) the estimate - gbar - the stochastic share (design 1.2's L_g); the base holds gbar.
//               The job records hold the difference (the resolve has the value and the share, so both follow from it).
//               Which is right is not settled by measurement: with the difference form the second hardware run showed
//               coloured blobs beside lamps; the suspected cause is that the difference carries the negative of the
//               control variate's own error (a probe map that sees a lamp the lobe does not), which a filter over
//               neighbours half corrects while gbar stays per pixel [suspected, to decide by capture];
//   base        what no spatial filter touches: an M hit's identity (emission, sun); for G see residual.
// Records. Per ray slot (the ray layers buffer, 16 B; ReflectionShadeRays): { stochastic rg, stochastic b | hit normal
// oct 8 + 8, albedo, hit instance | bit 31 a surface hit | bit 30 no cache data at the hit }. Per job (the job layers
// buffer, 24 B; ReflectionCombine and the inline path): { stochastic / albedo rg, b | hit normal, albedo, hit instance |
// bit 31 valid | bit 30 no cache data at half or more of its hits, residual rg, residual b }. Radiances are fp16 x
// REFL_STORE_SCALE as the results.
// No cache data: the lookup at the hit found no updated cell at any level it searches (ReflectionShade: the hit's
// indirect light is then 0 - after a cut, before the hit tier's first update). Such a pixel's stochastic layer is not an
// estimate of its light, so the spatial filter gives it its neighbours' value and does not spread its own (LayerDenoise;
// as GiScreenFilter treats pixels whose lookup found nothing).
#define REFL_LAYER_RAY_BYTES 16u
#define REFL_LAYER_JOB_BYTES 24u
#define REFL_LAYER_SURFACE 0x80000000u
#define REFL_LAYER_NO_DATA 0x40000000u
// Unit vector <-> octahedral 8 + 8 bits (a guide for agreement tests: 1.4 deg steps).
uint reflPackOct16(float3 n)
{
    const float3 a = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    float2 o = a.xy;
    if (a.z < 0) o = (1 - abs(a.yx)) * float2(a.x >= 0 ? 1 : -1, a.y >= 0 ? 1 : -1);
    const uint2 q = uint2(round(saturate(o * 0.5 + 0.5) * 255.0));
    return q.x | (q.y << 8);
}
float3 reflUnpackOct16(uint v)
{
    const float2 o = float2(v & 0xFFu, (v >> 8) & 0xFFu) / 255.0 * 2 - 1;
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}
// The demodulation albedo in 11 + 11 + 10 bits over [REFL_ALBEDO_MIN, 2] (diffuse + specular albedo can pass 1).
// The stored value is the one used on both sides (stochastic / albedo, albedo x filtered): its rounding cancels.
#define REFL_ALBEDO_MIN 0.03
uint reflPackAlbedo(float3 a)
{
    const float3 u = saturate(clamp(a, REFL_ALBEDO_MIN, 2.0) * 0.5);
    return (uint)round(u.r * 2047.0) | ((uint)round(u.g * 2047.0) << 11) | ((uint)round(u.b * 1023.0) << 22);
}
float3 reflUnpackAlbedo(uint v)
{
    return max(float3((v & 0x7FFu) / 2047.0, ((v >> 11) & 0x7FFu) / 2047.0, (v >> 22) / 1023.0) * 2.0, REFL_ALBEDO_MIN);
}
float3 reflLayerRadiance(uint2 v) { return float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) / (1.0 / 64.0); }
uint2 reflLayerPackRadiance(float3 r, uint high)
{
    const float3 s = clamp(r, -65504.0 * 64.0, 65504.0 * 64.0) * (1.0 / 64.0);
    return uint2(f32tof16(s.r) | (f32tof16(s.g) << 16), f32tof16(s.b) | (high << 16));
}
// A ray's layer record: its hit's stochastic part (modulated), albedo, shading normal and instance.
uint4 reflLayerRay(float3 stochastic, float3 albedo, float3 hitNormal, uint hitInstance, bool surface, bool noData = false)
{
    return uint4(reflLayerPackRadiance(stochastic, reflPackOct16(hitNormal)), reflPackAlbedo(albedo),
                 (hitInstance & 0x00FFFFFFu) | (surface ? REFL_LAYER_SURFACE : 0u) | (surface && noData ? REFL_LAYER_NO_DATA : 0u));
}
// A job's layers from its rays' sums (ReflectionCombine and the inline path: the same arithmetic). total = the job's value
// (reflLobeEstimate), gbar = the control variate's lobe integral (0 for M), sumS / sumA = the stochastic parts and albedos
// of its 'hits' surface hits among n unmasked rays, sumL / sumG as reflLobeEstimate takes them. The stochastic part's
// share of the value: mean(S) in the difference branch (the estimate is linear in the rays), mean(S) x gbar / mean(g)
// in a channel on the ratio branch (the estimate there is gbar mean(L) / mean(g)). The albedo is the hits' mean (the
// branch factor stays in the stochastic layer: a sample's branch is a noisy decision, averaged with the layer).
// residual = total - gbar - share (G: the difference form), 0 (M: base = total - share).
struct ReflJobLayers
{
    float3 stochastic;  // demodulated: share / albedo (the stored, quantised albedo)
    float3 residual;
    uint albedo;        // reflPackAlbedo
};
ReflJobLayers reflJobLayers(float3 total, float3 gbar, float3 sumL, float3 sumG, float3 sumS, float3 sumA, uint n, uint hits, bool glossy)
{
    ReflJobLayers o;
    o.stochastic = 0;
    o.residual = glossy ? total - gbar : float3(0, 0, 0);
    o.albedo = reflPackAlbedo(1.0);
    if (n == 0 || hits == 0) return o;
    float3 scale = 1;
    if (glossy && ((P[5].x >> 24) & 128) == 0)
    {
        const float3 meanG = sumG / n;
        scale = select(gbar + (sumL - sumG) / n >= 0, 1.0, gbar / max(meanG, 1e-8));
    }
    const float3 share = sumS / n * scale;
    o.albedo = reflPackAlbedo(sumA / hits);
    o.stochastic = share / reflUnpackAlbedo(o.albedo);
    if (glossy) o.residual = total - gbar - share;
    return o;
}

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
