#pragma once
// Sample generation. The first dimensions of every path use padded 2D Owen-scrambled Sobol points (Burley 2020,
// "Practical Hash-based Owen Scrambling"): each dimension pair gets its own scramble and index shuffle, so pairs are
// stratified and mutually independent. Deeper dimensions fall back to independent PCG32 numbers. Each half of the
// reference image uses a different seed, which makes the two halves independent estimates.
#include <cstdint>

namespace unx::reference
{
inline uint32_t hashU32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x21F0AAADu;
    x ^= x >> 15;
    x *= 0xD35A2D97u;
    x ^= x >> 15;
    return x;
}
inline uint32_t hashCombine(uint32_t seed, uint32_t v) { return seed ^ (hashU32(v) + 0x9E3779B9u + (seed << 6) + (seed >> 2)); }

inline uint32_t reverseBits(uint32_t x)
{
    x = (x << 16) | (x >> 16);
    x = ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x33333333u) << 2) | ((x & 0xCCCCCCCCu) >> 2);
    x = ((x & 0x55555555u) << 1) | ((x & 0xAAAAAAAAu) >> 1);
    return x;
}

// Laine-Karras style permutation of the bit-reversed value = nested uniform (Owen) scramble.
inline uint32_t owenScramble(uint32_t x, uint32_t seed)
{
    x = reverseBits(x);
    x += seed;
    x ^= x * 0x6C50B47Cu;
    x ^= x * 0xB82F1E52u;
    x ^= x * 0xC7AFE638u;
    x ^= x * 0x8D22F6E6u;
    return reverseBits(x);
}

// Sobol dimension 1 (the (0,2)-sequence partner of the van der Corput sequence): direction numbers v_k with m_k = 1.
inline uint32_t sobolDim1(uint32_t index)
{
    uint32_t v = 1u << 31, x = 0;
    for (; index; index >>= 1, v ^= v >> 1)
        if (index & 1) x ^= v;
    return x;
}

inline float toUnitFloat(uint32_t x) { return (float)(x >> 8) * (1.0f / 16777216.0f); }

struct Pcg32
{
    uint64_t state = 0, inc = 1;
    Pcg32() = default;
    Pcg32(uint64_t seed, uint64_t stream)
    {
        inc = (stream << 1u) | 1u;
        next();
        state += seed;
        next();
    }
    uint32_t next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u), rot = (uint32_t)(old >> 59u);
        return (xs >> rot) | (xs << ((32 - rot) & 31));
    }
    float uniform() { return toUnitFloat(next()); }
};

class Sampler
{
public:
    static constexpr uint32_t kSobolPairs = 24;  // camera + about three bounces of stratified dimensions

    Sampler(uint32_t pixelSeed, uint32_t sampleIndex) : m_seed(pixelSeed), m_index(sampleIndex), m_rng(((uint64_t)pixelSeed << 32) | sampleIndex, pixelSeed) {}

    float get1D()
    {
        if (m_pair < kSobolPairs)
        {
            const uint32_t s = hashCombine(m_seed, m_pair++);
            const uint32_t i = owenScramble(m_index, hashCombine(s, 0xA511E9B3u));
            return toUnitFloat(owenScramble(reverseBits(i), hashCombine(s, 1)));
        }
        return m_rng.uniform();
    }
    void get2D(float& u, float& v)
    {
        if (m_pair < kSobolPairs)
        {
            const uint32_t s = hashCombine(m_seed, m_pair++);
            const uint32_t i = owenScramble(m_index, hashCombine(s, 0xA511E9B3u));
            u = toUnitFloat(owenScramble(reverseBits(i), hashCombine(s, 1)));
            v = toUnitFloat(owenScramble(sobolDim1(i), hashCombine(s, 2)));
            return;
        }
        u = m_rng.uniform();
        v = m_rng.uniform();
    }

private:
    uint32_t m_seed, m_index, m_pair = 0;
    Pcg32 m_rng;
};
} // namespace unx::reference
