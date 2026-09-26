// Sample generation, bit-identical to Reference/PathTracer/src/Sampler.h (the CPU estimator): padded 2D Owen-scrambled
// Sobol points (Burley 2020) for the first kSobolPairs dimension pairs of a path, PCG32 beyond. Integer arithmetic
// only, so the C++ and HLSL compilations return the same bits (checked by unx_test_reference_gpu).
#ifndef UNX_RT_SAMPLER_HLSLI
#define UNX_RT_SAMPLER_HLSLI
#include "Compat.hlsli"

RT_BEGIN_NAMESPACE
RT_CONST uint kRtSobolPairs = 24;

RT_INLINE uint rtHashU32(uint x)
{
    x ^= x >> 16;
    x *= 0x21F0AAADu;
    x ^= x >> 15;
    x *= 0xD35A2D97u;
    x ^= x >> 15;
    return x;
}
RT_INLINE uint rtHashCombine(uint seed, uint v) { return seed ^ (rtHashU32(v) + 0x9E3779B9u + (seed << 6) + (seed >> 2)); }

RT_INLINE uint rtReverseBits(uint x)
{
    x = (x << 16) | (x >> 16);
    x = ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x33333333u) << 2) | ((x & 0xCCCCCCCCu) >> 2);
    x = ((x & 0x55555555u) << 1) | ((x & 0xAAAAAAAAu) >> 1);
    return x;
}

RT_INLINE uint rtOwenScramble(uint x, uint seed)
{
    x = rtReverseBits(x);
    x += seed;
    x ^= x * 0x6C50B47Cu;
    x ^= x * 0xB82F1E52u;
    x ^= x * 0xC7AFE638u;
    x ^= x * 0x8D22F6E6u;
    return rtReverseBits(x);
}

// Sobol dimension 1; the loop runs at most 32 times (one per index bit).
RT_INLINE uint rtSobolDim1(uint index)
{
    uint v = 1u << 31, x = 0;
    for (uint k = 0; k < 32 && index != 0; ++k)
    {
        if ((index & 1u) != 0) x ^= v;
        index >>= 1;
        v ^= v >> 1;
    }
    return x;
}

RT_INLINE float rtToUnitFloat(uint x) { return (float)(x >> 8) * (1.0f / 16777216.0f); }

struct RtPcg32
{
    uint64_t state;
    uint64_t inc;
};
RT_INLINE uint rtPcgNext(RT_INOUT(RtPcg32) g)
{
    const uint64_t old = g.state;
    g.state = old * 6364136223846793005ull + g.inc;
    const uint xs = (uint)(((old >> 18u) ^ old) >> 27u), rot = (uint)(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}
RT_INLINE RtPcg32 rtPcgInit(uint64_t seed, uint64_t stream)
{
    RtPcg32 g;
    g.state = 0;
    g.inc = (stream << 1u) | 1u;
    rtPcgNext(g);
    g.state += seed;
    rtPcgNext(g);
    return g;
}
RT_INLINE float rtPcgUniform(RT_INOUT(RtPcg32) g) { return rtToUnitFloat(rtPcgNext(g)); }

struct RtSampler
{
    uint seed;
    uint index;
    uint pair;
    RtPcg32 rng;
};
RT_INLINE RtSampler rtSamplerInit(uint pixelSeed, uint sampleIndex)
{
    RtSampler s;
    s.seed = pixelSeed;
    s.index = sampleIndex;
    s.pair = 0;
    s.rng = rtPcgInit(((uint64_t)pixelSeed << 32) | (uint64_t)sampleIndex, (uint64_t)pixelSeed);
    return s;
}
RT_INLINE float rtGet1D(RT_INOUT(RtSampler) s)
{
    if (s.pair < kRtSobolPairs)
    {
        const uint h = rtHashCombine(s.seed, s.pair);
        s.pair += 1;
        const uint i = rtOwenScramble(s.index, rtHashCombine(h, 0xA511E9B3u));
        return rtToUnitFloat(rtOwenScramble(rtReverseBits(i), rtHashCombine(h, 1u)));
    }
    return rtPcgUniform(s.rng);
}
RT_INLINE void rtGet2D(RT_INOUT(RtSampler) s, RT_OUT(float) u, RT_OUT(float) v)
{
    if (s.pair < kRtSobolPairs)
    {
        const uint h = rtHashCombine(s.seed, s.pair);
        s.pair += 1;
        const uint i = rtOwenScramble(s.index, rtHashCombine(h, 0xA511E9B3u));
        u = rtToUnitFloat(rtOwenScramble(rtReverseBits(i), rtHashCombine(h, 1u)));
        v = rtToUnitFloat(rtOwenScramble(rtSobolDim1(i), rtHashCombine(h, 2u)));
        return;
    }
    u = rtPcgUniform(s.rng);
    v = rtPcgUniform(s.rng);
}
RT_END_NAMESPACE

#endif
