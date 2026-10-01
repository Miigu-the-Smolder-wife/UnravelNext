// unx-kernel: cs_6_6 main
// Start of the frame's VSM update: zeroed counters (the page list count is VsmScan's).
// P[0].y stats UAV (raw)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint i = 0; i < 32; ++i) stats.Store4(i * 16, uint4(0, 0, 0, 0));  // 128 words (VsmSystem.cpp kStatsBytes)
}
