// The ray-reuse reflection pipeline (reflection.lumen; ReflectionSystem.cpp): the structure of Unreal Engine's Lumen
// reflections (ue6-main: LumenReflections.usf, LumenReflectionResolve.usf, LumenReflectionDenoiserTemporal.usf,
// LumenReflectionDenoiserSpatial.usf read as a reference; no code taken) built on this renderer's passes:
//   classification  a pixel whose roughness is below reflection.lumen_max_roughness_to_trace traces one ray drawn from
//                   its GGX lobe (the M job of ReflectionClassify / ReflectionTrace; real materials shaded at the hits);
//                   rougher pixels trace nothing and take the K path (the screen probes' lobe radiance, as Lumen takes
//                   its final gather's rough specular); over the last lumen_roughness_fade_length of roughness the two
//                   are mixed (ShadeOpaque: the reflection's alpha).
//   resolve         ReflectionReuseResolve: each pixel's value from its own ray and its neighbours' rays, a neighbour's
//                   hit point seen from this pixel and weighted by this pixel's lobe over the ray's density.
//   time            ReflectionReuseTemporal: at most lumen_temporal_max_frames frames, the history taken where the
//                   surface was or where the reflected image was, bounded by the resolved neighbourhood's spread.
//   filter          ReflectionReuseFilter: a bilateral filter steered by the temporal variance, wider on the frames
//                   without history.
// Planar mirrors and calm water keep the raster reflection cameras (ReflectionResolve writes them).
// Values that give up accuracy for quietness, as the reference does (Config/quality/reflection.toml; 0 turns each off):
// the ray intensity cap (lumen_max_ray_intensity), the averaging in a tone-mapped space (lumen_tonemap_range) and the
// stronger tone map on frames without history (lumen_disocclusion_tonemap).
#ifndef UNX_REFLECTION_REUSE_HLSLI
#define UNX_REFLECTION_REUSE_HLSLI
#include "Passes/Reflection/ReflectionInternal.hlsli"

float reuseLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The filters' working space: exposed radiance (display-linear: nits x g_exposure) compressed by its luminance over
// 'range' - a mean taken there weighs a sample by 1 / (1 + luminance / range), so a few very bright samples cannot own
// it. range <= 0: plain exposed radiance (an energy-preserving mean).
float3 reuseToFilter(float3 radiance, float range)
{
    const float3 v = radiance * g_exposure;
    return range > 0 ? v / (1 + reuseLuminance(v) / range) : v;
}
float3 reuseFromFilter(float3 v, float range)
{
    if (range > 0) v = v / max(1 - reuseLuminance(v) / range, 1e-4);
    return v / max(g_exposure, 1e-20);
}

// A ray's radiance held to 'cap' in exposed units (its largest channel); cap <= 0: unchanged.
float3 reuseCapIntensity(float3 radiance, float cap)
{
    const float m = max(radiance.r, max(radiance.g, radiance.b)) * g_exposure;
    return cap > 0 && m > cap ? radiance * (cap / m) : radiance;
}

float3 reuseToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 reuseFromYCoCg(float3 y)
{
    const float t = y.x - y.z;
    return float3(t + y.y, y.x + y.z, t - y.y);
}

// The specular lobe's half-angle that holds three quarters of its energy, and how far the lobe's dominant direction
// follows the mirror direction (1: a mirror; Frostbite's fit) - the measures the reference's history and filter use.
float reuseLobeHalfAngle(float roughness) { return atan(3.0 * roughness * roughness); }
float reuseDominantDirection(float roughness)
{
    const float s = saturate(1 - roughness);
    return s * (sqrt(s) + roughness);
}

// The frames a pixel may accumulate: a mirror needs few (and ghosts with more).
float reuseMaxFrames(float maxFrames, float roughness) { return lerp(min(2.0, maxFrames), maxFrames, saturate(roughness / 0.05)); }

// Share of the traced reflection in the pixel's specular light (the rest is the K path's lobe radiance).
float reuseTraceShare(float roughness, float maxRoughness, float fadeLength) { return saturate((maxRoughness - roughness) / max(fadeLength, 1e-4)); }

uint reuseHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float reuseUnit(uint x) { return (reuseHash(x) >> 8) * (1.0 / 16777216.0); }

// Point i of n on the unit disk, uniform in area: a golden-angle spiral turned and shifted per pixel and frame.
float2 reuseDisk(uint i, uint n, float2 u)
{
    const float r = sqrt((i + u.x) / n);
    const float phi = 6.28318530718 * u.y + 2.39996322973 * i;
    return r * float2(cos(phi), sin(phi));
}

float reuseGgxD(float a2, float NoH)
{
    const float d = NoH * NoH * (a2 - 1) + 1;
    return a2 / (3.14159265 * d * d);
}

// The ray an M job of 'pixel' traced this frame (ReflectionRay.hlsli: reflPixelSeed, reflNextDirection - the same draws)
// and its density over the reflected directions (the visible-normal distribution's: G1(v) D(h) / (4 n.v)).
// False when every attempt was masked (no ray).
bool reuseRay(ReflSurface s, uint2 pixel, uint frame, out float3 dir, out float pdf)
{
    const float alpha = max(s.roughness * s.roughness, 1e-4);
    uint seed = reuseHash(pixel.x * 7919u + pixel.y * 104729u + (frame & 0xFFFFFFu) * 15485863u);
    dir = 0;
    pdf = 0;
    bool found = false;
    [loop] for (uint attempt = 0; attempt < 8 && !found; ++attempt)
    {
        const float2 u = float2(reuseUnit(seed), reuseUnit(seed + 1));
        seed = reuseHash(seed + 2);
        dir = reflSampleGgx(s.normal, s.view, alpha, u);
        found = dot(dir, s.normal) > 0;
    }
    if (!found) return false;
    const float a2 = alpha * alpha;
    const float NoV = max(dot(s.normal, s.view), 1e-4);
    const float NoH = saturate(dot(s.normal, normalize(s.view + dir)));
    const float g1 = 2 * NoV / (NoV + sqrt(a2 + (1 - a2) * NoV * NoV));
    pdf = max(g1 * reuseGgxD(a2, NoH) / (4 * NoV), 1e-6);
    return true;
}

#endif
