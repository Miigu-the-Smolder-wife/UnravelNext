// unx-kernel: cs_6_6 main
// Froxel light lists, start of the frame: the grid header (FroxelCommon.hlsli) with zeroed allocation and statistics.
// P[0].x froxelLights UAV (raw), P[0].y gridX, P[0].z gridY, P[0].w slices
// P[1].x tilePx, P[1].y nearM (float bits), P[1].z farM (float bits), P[1].w index stride (entries per froxel, even)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const uint froxels = P[0].y * P[0].z * P[0].w;
    const float nearM = asfloat(P[1].y), farM = asfloat(P[1].z);
    b.Store4(0, uint4(P[0].y, P[0].z, P[0].w, P[1].x));
    b.Store4(16, uint4(P[1].y, P[1].z, asuint(log2(farM / nearM)), 0));
    b.Store4(32, uint4(64, 64 + froxels * 4, P[1].w, 0));
    b.Store4(48, uint4(0, 0, 0, 0));
}
