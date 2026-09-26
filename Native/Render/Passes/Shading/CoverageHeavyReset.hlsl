// unx-kernel: cs_6_6 main
// Coverage composite between the heavy rounds' two parts (CoverageComposite.hlsl, CoverageHeavyRound.hlsl PART): one
// thread per heavy pixel puts its merge back to the start - run cursors at their runs' first pairs, mask union, weight
// sum, done flag and last key cleared - keeping part 1's radiance sum; thread 0 empties active list 1 (part 2's round 0
// appends to it). Runs per pixel <= its fragments / COV_BLOCK (the pool's bound).
// P[0] = { state UAV (raw), heavy records UAV (raw), run cursors UAV (raw), heavy capacity }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer heavy = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer cursors = ResourceDescriptorHeap[P[0].z];
    const uint h = (gid.x + gid.y * 65535) * 64 + gi;
    if (h == 0) state.Store(4 * (COVS_OPEN + 1), 0);
    if (h >= min(state.Load(4 * COVS_HEAVY), P[0].w)) return;
    const uint base = 4 * h * COVH_WORDS;
    const uint4 rec0 = heavy.Load4(base);  // pixel, segment, count, runs
    const uint cursor = heavy.Load(base + 4 * COVH_CURSORS);
    [loop] for (uint run = 0; run < rec0.w; ++run) cursors.Store(4 * (cursor + run), rec0.y + run * COV_BLOCK);
    heavy.Store3(base + 4 * COVH_COVERED, uint3(0, asuint(0.0), 0));  // covered, used, done
    heavy.Store(base + 4 * COVH_LAST, 0xFFFFFFFFu);
    heavy.Store(base + 4 * (COVH_LAST + 1), 0);
}
