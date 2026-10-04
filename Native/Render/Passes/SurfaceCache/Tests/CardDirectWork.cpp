// CPU-only identity check. The helper is the production HLSL/C++ shared source;
// compare surviving original threads, not list order (the GPU append is atomic).
#include "../CardDirectWork.h"
#include <array>
#include <cstdio>
using namespace unx::render::refl::detail;
static_assert(clDirectWorkThread(clDirectWorkPack(65534, 8, 3, false), 15) == 65534u * 576 + 575);
static_assert(clDirectWorkThread(clDirectWorkPack(65534, 8, 0, true), 15) == 65534u * 576 + 512 + 54);
static_assert(!clDirectWorkPresent(0xAAAAAAAAu, 0xAAAAAAAAu, 0, true));
static_assert(clDirectWorkPresent(0x00550055u, 0, 0, true));
int main()
{
    uint cases = 0;
    for (uint seed = 0; seed < 4096; ++seed)
    {
        uint lo = seed * 1664525u + 1013904223u, hi = lo * 1664525u + 1013904223u;
        // Exhaust every single live/dead texel as well as dense/random masks.
        if (seed < 64) { lo = seed < 32 ? 1u << seed : 0; hi = seed >= 32 ? 1u << (seed - 32) : 0; }
        if (seed == 64) lo = hi = 0;
        if (seed == 65) lo = hi = 0xFFFFFFFFu;
        for (uint slot = 0; slot < 9; ++slot)
        for (uint coarse = 0; coarse < 2; ++coarse)
        {
            std::array<uint, 64> seen{};
            uint blocks = 0;
            for (uint quarter = 0; quarter < (coarse ? 1u : 4u); ++quarter)
            {
                if (!clDirectWorkPresent(lo, hi, quarter, coarse != 0)) continue;
                ++blocks;
                const uint record = clDirectWorkPack(65534, slot, quarter, coarse != 0);
                for (uint lane = 0; lane < 16; ++lane)
                {
                    const uint original = clDirectWorkThread(record, lane);
                    const uint base = 65534u * 576 + slot * 64;
                    if (original < base || original >= base + 64) return 1;
                    const uint t = original - base;
                    if (((t < 32 ? lo >> t : hi >> (t - 32)) & 1u) != 0) ++seen[t];
                }
            }
            for (uint t = 0; t < 64; ++t)
            {
                const bool valid = ((t < 32 ? lo >> t : hi >> (t - 32)) & 1u) != 0;
                const bool originalRuns = valid && (!coarse || (t & 9u) == 0);
                if (seen[t] != uint(originalRuns)) return 2;
            }
            if (blocks > (coarse ? 1u : 4u)) return 3;
            ++cases;
        }
    }
    std::printf("PASS card direct work: %u mask/slot/mode cases, original live ray identities occur exactly once; CPU only\n", cases);
    return 0;
}
