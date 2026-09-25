// unx-kernel: cs_6_6 main
// Start of the frame's VSM update: empty dirty and moved lists, zeroed counters.
// P[0].x dirty list UAV (raw), P[0].y stats UAV (raw), P[0].z moved list UAV (raw)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].y];
    dirty.Store2(0, uint2(0, 0));
    RWByteAddressBuffer moved = ResourceDescriptorHeap[P[0].z];
    moved.Store(0, 0);
    stats.Store4(0, uint4(0, 0, 0, 0));
    stats.Store4(16, uint4(0, 0, 0, 0));
    stats.Store4(32, uint4(0, 0, 0, 0));
    stats.Store4(48, uint4(0, 0, 0, 0));
    stats.Store4(64, uint4(0, 0, 0, 0));
    stats.Store4(80, uint4(0, 0, 0, 0));
    stats.Store4(96, uint4(0, 0, 0, 0));
    stats.Store4(112, uint4(0, 0, 0, 0));
    [unroll] for (uint i = 0; i < 8; ++i) stats.Store4(128 + i * 16, uint4(0, 0, 0, 0));
}
