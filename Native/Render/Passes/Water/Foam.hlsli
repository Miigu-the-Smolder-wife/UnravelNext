// Water foam F (FEATURES_GAME 1.3 (f)-F): F(x0, t) = max over t' <= t of C(x0, t') exp(-(t - t') / tau), the breaking
// fraction C = P(J < J_t) = Phi((J_t - Jbar) / sigma_J) at a level's texel (Jbar the Jacobian low-passed to the texel,
// sigma_J^2 the divergence variance of the bands the texel removes). Stored on a world-space clipmap: level l, texel
// s_l = s_0 4^l, 1024^2 texels with toroidal addressing (world texel I at storage (I + bias) mod 1024; the bias keeps a
// texel's storage across origin rebases, Foam::rebase), window [origin, origin + 1024) around the camera. Texel I covers
// rest positions [I s_l, (I + 1) s_l).
// Parameter buffer (raw): row 0 levels, s_0 (m), tau (s), J_t; per level l, from byte 16 + 32 l: origin (int x, z),
// previous origin (int x, z), elapsed time since its last update (s), updated this frame (uint), storage bias (int x, z).
#ifndef UNX_WATER_FOAM_HLSLI
#define UNX_WATER_FOAM_HLSLI
#include "Bindless.hlsli"

#define FOAM_N 1024
#define FOAM_LEVELS 5

struct FoamLevel
{
    int2 origin, previous;
    float elapsed;
    uint updated;
    int2 bias;
};
struct FoamParams
{
    uint levels;
    float s0, tau, threshold;
    uint srv;
};
FoamParams foamParams(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    const uint4 r = b.Load4(0);
    FoamParams p;
    p.levels = r.x; p.s0 = asfloat(r.y); p.tau = asfloat(r.z); p.threshold = asfloat(r.w);
    p.srv = srv;
    return p;
}
FoamLevel foamLevel(FoamParams p, uint level)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    const uint4 r0 = b.Load4(16 + 32 * level), r1 = b.Load4(32 + 32 * level);
    FoamLevel l;
    l.origin = asint(r0.xy); l.previous = asint(r0.zw); l.elapsed = asfloat(r1.x); l.updated = r1.y; l.bias = asint(r1.zw);
    return l;
}
float foamSpacing(FoamParams p, uint level) { return p.s0 * float(1u << (2 * level)); }
uint2 foamStorage(FoamLevel l, int2 worldTexel) { return uint2((worldTexel + l.bias) & (FOAM_N - 1)); }

// Standard normal CDF: 0.5 erfc(-x / sqrt 2) with Abramowitz-Stegun 7.1.26 (|error| <= 1.5e-7).
float foamPhi(float x)
{
    const float z = abs(x) * 0.70710678, t = 1.0 / (1.0 + 0.3275911 * z);
    const float e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) * exp(-z * z);
    return x >= 0 ? 1.0 - 0.5 * e : 0.5 * e;
}

// Foam at rest position x0 on level l, bilinear over texel centres ((I + 0.5) s_l); false outside the level's window.
bool foamLevelSample(Texture2DArray<float> foam, FoamParams p, uint level, float2 x0, out float value)
{
    const FoamLevel l = foamLevel(p, level);
    const float2 u = x0 / foamSpacing(p, level) - 0.5;
    const int2 i0 = int2(floor(u));
    const float2 f = u - float2(i0);
    value = 0;
    if (any(i0 < l.origin) || any(i0 + 1 >= l.origin + FOAM_N)) return false;
    const float a = foam.Load(int4(foamStorage(l, i0), level, 0)), b = foam.Load(int4(foamStorage(l, i0 + int2(1, 0)), level, 0));
    const float c = foam.Load(int4(foamStorage(l, i0 + int2(0, 1)), level, 0)), d = foam.Load(int4(foamStorage(l, i0 + 1), level, 0));
    value = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    return true;
}
// Foam for shading at rest position x0 and pixel footprint (m): the two levels around the footprint blended (their
// breaking fractions share one expectation), the coarser one near a window's edge.
float foamAt(uint foamSrv, uint paramSrv, float2 x0, float footprint)
{
    Texture2DArray<float> foam = ResourceDescriptorHeap[foamSrv];
    const FoamParams p = foamParams(paramSrv);
    const float position = clamp(log2(max(footprint, 1e-6) / p.s0) * 0.5, 0.0, float(p.levels - 1));  // log4
    uint level = uint(position);
    float fraction = position - float(level);
    float value, coarser;
    [loop] while (level < p.levels && !foamLevelSample(foam, p, level, x0, value)) { ++level; fraction = 0; }
    if (level >= p.levels) return 0;
    if (level + 1 < p.levels && foamLevelSample(foam, p, level + 1, x0, coarser))
    {
        // Within the last tenth of the window towards its edge, hand over to the coarser level.
        const FoamLevel l = foamLevel(p, level);
        const float2 u = x0 / foamSpacing(p, level) - float2(l.origin);
        const float edge = min(min(u.x, u.y), min(FOAM_N - u.x, FOAM_N - u.y)) / (0.1 * FOAM_N);
        return lerp(coarser, value, saturate(edge) * (1.0 - fraction));
    }
    return value;
}
#endif
