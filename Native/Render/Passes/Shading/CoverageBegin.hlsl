// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// Coverage composite state (CoverageShade.hlsli), one thread.
//   MODE=0: reset the state and argument words before the frame's stages.
//   MODE=1: after CoverageComposite: the heavy passes' dispatch arguments (heavy pixels in x up to 65535, then y; the run
//           sort's z = the most runs of one pixel), clamped to the heavy records' capacity. Round 0 takes every heavy pixel.
//   MODE=2: before heavy round r >= 1 (P[0].z = r): its arguments from the pixels round r - 1 left open (active list
//           r % 2), and the list round r appends to ((r + 1) % 2) emptied.
// P[0] = { state UAV (raw), heavy capacity (records), round (MODE=2), arguments UAV (raw) }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

uint3 covGrid(uint n) { return uint3(min(n, 65535u), n == 0 ? 0 : (n + 65534) / 65535, 1); }

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].w];
#if MODE == 0
    [unroll] for (uint i = 0; i < COVS_WORDS; i += 4) state.Store4(4 * i, uint4(0, 0, 0, 0));
    [unroll] for (uint j = 0; j < COVA_WORDS; j += 4) args.Store4(4 * j, uint4(0, 0, 0, 0));
#elif MODE == 1
    const uint heavy = min(state.Load(4 * COVS_HEAVY), P[0].y);
    const uint3 grid = covGrid(heavy);
    args.Store3(4 * COVA_SORT, uint3(grid.xy, heavy == 0 ? 0 : state.Load(4 * COVS_MAX_RUNS)));
    args.Store3(4 * COVA_ROUND, grid);
    args.Store3(4 * COVA_FINISH, covGrid((heavy + 63) / 64));
#else
    const uint r = P[0].z;
    args.Store3(4 * COVA_ROUND, covGrid(state.Load(4 * (COVS_OPEN + r % 2))));
    state.Store(4 * (COVS_OPEN + (r + 1) % 2), 0);
#endif
}
