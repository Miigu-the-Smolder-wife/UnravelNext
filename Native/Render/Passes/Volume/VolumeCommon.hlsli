// Particle media and heat haze (track E; request 20260925_FX_particle_render_rules 3b, FEATURES_GAME 0.A-8). Owner: E.
//
// Per frame and view, from the FX particle module's latest two ticks (ParticleSystem::renderInputs):
//  - every volume-output particle at the frame time (the sprites' interpolation, FxLayerSetup.hlsl) becomes a media record:
//    its tent-mass density rho(x) = m prod_i max(0, 1 - |x_i - c_i| / r) / r^3 (r = size / 2, m = alpha; int rho dV = m),
//    extinction per unit density (medium_absorption + medium_scattering, rgb) and source per unit density (the
//    in-scattered radiance medium_scattering x L_in + the emission medium_emission x colour), L_in lit once at the particle
//    centre with its phase (the lit sprites' law, fxLitRadiance); binned to the froxel tiles it overlaps (VolumeSlices);
//  - every distortion-output particle becomes a haze record binned to 1/4-resolution tiles (VolumeDistortion).
#ifndef UNX_VOLUME_COMMON_HLSLI
#define UNX_VOLUME_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define VOLUME_HAZE_TILE 8u          // 1/4-resolution texels per haze tile side
#define VOLUME_HAZE_SCALE 4u         // full-resolution pixels per haze texel side
#define VOLUME_KIND_NONE 0u
#define VOLUME_KIND_MEDIA 1u
#define VOLUME_KIND_HAZE 2u
#define VOLUME_COUNTER_MEDIA_ENTRIES 0u
#define VOLUME_COUNTER_HAZE_ENTRIES 1u
#define VOLUME_COUNTER_STATUS 2u
#define VOLUME_COUNTER_RECORDS 3u          // media and haze records of the frame
#define VOLUME_STATUS_MEDIA_OVERFLOW 1u    // media tile entries exceed their buffer (entries dropped: a defect)
#define VOLUME_STATUS_HAZE_OVERFLOW 2u     // haze tile entries exceed their buffer
#define VOLUME_STATUS_RANGE 4u             // an index outside its buffer
#define VOLUME_STATUS_HAZE_LARGE 8u        // |D| >= 8 px somewhere: outside the linear small-angle condition (reported)
#define VOLUME_STATUS_NONFINITE 16u        // a record or a slice integral with a NaN or infinity (a defect upstream): the
                                           // record is not binned and a slice keeps no medium, never full opacity

struct VolumeConstants
{
    float3 offsetCur; float w;          // stream anchor of the latest tick - camera; frame time in the latest tick (0..1)
    float3 offsetPrev; float dt;
    uint threads, current, rangeCount, mode;  // mode: bit 0 haze, bit 1 media
    uint posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint dynamicCur, dynamicPrev, emitters, programs;
    uint curveKeys, ranges, blocks, records;
    uint hazeWidth, hazeHeight, hazeTilesX, hazeTilesY;
    uint hazeCounts, hazeStarts, hazeFill, hazeEntries;
    uint hazeEntryCapacity, distortionOffset, distortionDepth, counters;
    uint froxelLights, mediaCounts, mediaStarts, mediaFill;
    uint mediaEntries, mediaEntryCapacity, volumeSlices, mediaTiles;
    uint shadow[8];                     // S's ShadowSrvs (page table, pool/atlas, blocks, search bound, constants, lights,
                                        // slot of light, layers)
    uint giCache, airVolume, transmittance, multiScatter;
    float3 streamAxes; uint hazeCells;  // stream (VFX World) -> renderer axis signs (FrameContext::streamAxes): the offsets and
                                        // the particles are in stream space, volumeParticleAt maps each camera-relative
                                        // position; hazeCells: the haze lists' cells (mediaTiles: the media lists' cells)
};

// ---- Tile lists as a loose quadtree (media on the froxel tiles, haze on the haze tiles). A record's tile rectangle
// [t0, t1] is listed at the smallest level L whose cells (2^L x 2^L tiles) it spans at most K x K of (K =
// VOLUME_LOOSE_SPAN), so a record has at most K^2 entries whatever its size: the entry buffer's capacity K^2 x records is
// exact and no entry is ever dropped (a per-tile list needs one entry per covered tile; a puff near the camera covers
// thousands). A tile reads the cell holding it at every level and keeps the entries whose rectangle holds it; the tiles a
// record's cells cover beyond its rectangle are at most ((K + 1) / (K - 1))^2 of it (K = 4: 2.8x worst, the entry's
// rectangle test only). Levels are concatenated from L = 0 (the tiles) to the first 1 x 1.
// Entry: uint4 (record, z0 | z1 as halves (media), t0.x | t0.y << 16, t1.x | t1.y << 16).
#define VOLUME_LOOSE_SPAN 4u  // VolumePass.cpp kLooseSpan
uint2 volumeLevelDims(uint2 tiles, uint L) { return (tiles + (1u << L) - 1u) >> L; }
uint volumeLevelCount(uint2 tiles)
{
    uint L = 0;
    [loop] while (any(volumeLevelDims(tiles, L) > 1u)) ++L;
    return L + 1;
}
uint volumeLevelBase(uint2 tiles, uint L)
{
    uint base = 0;
    [loop] for (uint k = 0; k < L; ++k)
    {
        const uint2 d = volumeLevelDims(tiles, k);
        base += d.x * d.y;
    }
    return base;
}
uint volumeLevelOf(uint2 t0, uint2 t1)
{
    uint L = 0;
    [loop] while (any((t1 >> L) - (t0 >> L) >= VOLUME_LOOSE_SPAN)) ++L;
    return L;
}
bool volumeEntryHolds(uint4 e, uint2 tile)
{
    return all(tile >= uint2(e.z & 0xFFFFu, e.z >> 16)) && all(tile <= uint2(e.w & 0xFFFFu, e.w >> 16));
}

// One particle of the frame, 48 B (index = render thread).
struct VolumeRecord
{
    float3 centre; float radius;   // camera-relative position (m); media: tent half-width r; haze: profile size s / shell radius R
    float3 a; float mass;          // media: extinction per unit density (rgb); haze: (index amplitude, shell width fraction,
                                   // profile 0 blob / 1 shell); media: m; haze: support radius (m)
    float3 b; uint kind;           // media: source per unit density (nit / m per unit density, rgb)
};

VolumeConstants volumeConstants()
{
    StructuredBuffer<VolumeConstants> c = ResourceDescriptorHeap[P[0].x];
    return c[0];
}
void volumeStatus(VolumeConstants c, uint bits)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    InterlockedOr(counters[VOLUME_COUNTER_STATUS], bits);
}

// ---- Tent-mass line integral: int_a^b prod_i max(0, 1 - |o_i + d_i t - c_i| / r) dt, exact.
// Each factor is piecewise linear in t with breakpoints where o_i + d_i t - c_i is -r, 0 or r; between consecutive
// breakpoints the product is a cubic, which Simpson's rule integrates exactly.
float volumeTentFactor(float3 o, float3 d, float t, float3 c, float r)
{
    const float3 q = 1.0f - abs(o + d * t - c) / r;
    return max(q.x, 0.0f) * max(q.y, 0.0f) * max(q.z, 0.0f);
}
float volumeTentLine(float3 o, float3 d, float a, float b, float3 c, float r)
{
    if (!(b > a) || !(r > 0)) return 0;
    float bp[11];
    uint n = 0;
    bp[n++] = a;
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        const float di = d[i];
        if (abs(di) < 1e-12f)
        {
            if (abs(o[i] - c[i]) >= r) return 0;  // the line never enters this axis' support
            continue;
        }
        [unroll] for (int k = -1; k <= 1; ++k)
        {
            const float t = (c[i] - o[i] + k * r) / di;
            if (t > a && t < b) bp[n++] = t;
        }
    }
    bp[n++] = b;
    // insertion sort (n <= 11)
    for (uint x = 1; x < n; ++x)
    {
        const float v = bp[x];
        uint y = x;
        for (; y > 0 && bp[y - 1] > v; --y) bp[y] = bp[y - 1];
        bp[y] = v;
    }
    float sum = 0;
    for (uint s = 0; s + 1 < n; ++s)
    {
        const float t0 = bp[s], t1 = bp[s + 1], h = t1 - t0;
        if (h <= 0) continue;
        sum += h / 6.0f * (volumeTentFactor(o, d, t0, c, r) + 4.0f * volumeTentFactor(o, d, 0.5f * (t0 + t1), c, r) + volumeTentFactor(o, d, t1, c, r));
    }
    return sum;
}

// ---- Heat haze: transverse gradient of the view ray's integrated refractive-index perturbation at impact parameter b
// (the ray bends by theta = G(b) towards the particle axis for G < 0; small-angle, thin deflector).
//  blob:  dn = A exp(-d^2 / s^2)             -> N(b) = A s sqrt(pi) exp(-b^2 / s^2), G = -2 A sqrt(pi) (b / s) exp(-b^2 / s^2)
//  shell: dn = A exp(-(d - R)^2 / w^2), w = f R -> G(b) = int (d dn / dd)(r) (b / r) dl over the chord, r = sqrt(b^2 + l^2),
//         on the part of the chord that crosses the shell [R - 4w, R + 4w] (the rest carries dn < e^-16 A), in u = sqrt(r - b).
static const float kGl16X[8] = { 0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                                 0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499 };
static const float kGl16W[8] = { 0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                                 0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541 };
float volumeHazeGradient(float b, float radius, float3 params)
{
    const float A = params.x;
    if (params.z < 0.5f)
    {
        const float s = radius, x = b / s;
        return -2.0f * A * 1.7724538509055159f * x * exp(-x * x);
    }
    const float R = radius, w = max(params.y * R, 1e-6f);
    const float r0 = max(R - 4.0f * w, b), r1 = R + 4.0f * w;
    if (!(r1 > b)) return 0;
    // r = b + u^2 removes the chord's 1 / sqrt(r^2 - b^2) singularity at the tangent point:
    // G = 2 b int f'(r) / sqrt(r^2 - b^2) dr = 4 b int f'(b + u^2) / sqrt(2 b + u^2) du, u in [sqrt(r0 - b), sqrt(r1 - b)],
    // two panels of Gauss-Legendre 16.
    const float u0 = sqrt(r0 - b), u1 = sqrt(r1 - b);
    float sum = 0;
    [unroll] for (uint panel = 0; panel < 2u; ++panel)
    {
        const float a0 = u0 + (u1 - u0) * 0.5f * panel, a1 = a0 + (u1 - u0) * 0.5f;
        const float halfSpan = 0.5f * (a1 - a0), mid = 0.5f * (a1 + a0);
        [unroll] for (uint k = 0; k < 8; ++k)
            [unroll] for (int sgn = -1; sgn <= 1; sgn += 2)
            {
                const float u = mid + sgn * halfSpan * kGl16X[k];
                const float x = (b + u * u - R) / w;
                const float dndr = A * exp(-x * x) * (-2.0f * x / w);
                sum += kGl16W[k] * halfSpan * dndr * rsqrt(max(2.0f * b + u * u, 1e-20f));
            }
    }
    return 4.0f * b * sum;
}
float volumeHazeSupport(float radius, float3 params) { return params.z < 0.5f ? 3.0f * radius : radius * (1.0f + 4.0f * params.y); }

// Full-resolution pixel of a camera-relative direction (the view's rotation and projection).
float2 volumePixelOf(float3 dir)
{
    const float3 v = mul((float3x3)g_view, dir);
    const float4 clip = mul(g_proj, float4(v, 1));
    const float2 ndc = clip.xy / clip.w;
    return float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight);
}
// Camera-relative view ray through a full-resolution pixel, scaled to unit view depth.
float3 volumeRayAt(float2 pixel)
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, 1, 1));  // device depth 1 = view depth g_nearPlane
    return (p.xyz / p.w - g_cameraPosition) / g_nearPlane;
}
#endif
