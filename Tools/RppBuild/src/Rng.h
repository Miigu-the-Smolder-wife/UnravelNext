#pragma once
// Deterministic streams for RppBuild (manifest "seeds"): SplitMix64(master, fnv1a64(name)) seeds a PCG32 per purpose, so
// adding content to one stream never reshuffles another. Integer arithmetic only (no std:: distributions).
#include <cstdint>
#include <string_view>

namespace unx::rpp
{
inline uint64_t fnv1a64(std::string_view s)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : s) h = (h ^ (uint8_t)c) * 1099511628211ull;
    return h;
}
inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
struct Rng
{
    uint64_t state = 0, inc = 1;
    Rng(uint64_t master, std::string_view stream)
    {
        const uint64_t s = splitmix64(master ^ fnv1a64(stream));
        inc = (splitmix64(s) << 1) | 1u;
        state = 0;
        next();
        state += s;
        next();
    }
    uint32_t next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-(int32_t)rot) & 31));
    }
    float uniform() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
    uint32_t below(uint32_t n) { return (uint32_t)(((uint64_t)next() * n) >> 32); }
};
} // namespace unx::rpp
