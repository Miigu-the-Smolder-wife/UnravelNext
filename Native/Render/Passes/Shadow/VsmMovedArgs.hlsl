// unx-kernel: cs_6_6 main
// Indirect arguments of VsmInvalidate: one group per (moved instance, clipmap level); of VsmLocalInvalidate: one group
// per (moved instance, raster-active local light).
// P[0].x moved list SRV (raw), P[0].y indirect args UAV (raw, dispatches at bytes 16 and 32), P[0].z active local lights
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer moved = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    args.Store3(16, uint3(moved.Load(0), VSM_LEVELS, 1));
    args.Store3(32, uint3(moved.Load(0), max(P[0].z, 1u), 1));
}
