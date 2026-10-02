// unx-kernel: cs_6_6 main
// One lane per queued slice across tiles. Every lane runs the original ordered
// substeps and shadow walk; the consumer performs the original tile scans and
// local-light sums. P4.z = queue SRV, P4.w = FP32 air scratch UAV.
#include "Passes/Atmosphere/FroxelSlice.hlsli"
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer queue = ResourceDescriptorHeap[P[4].z];
    const uint item = (group.y * 65535u + group.x) * 64u + lane;
    if (item >= queue.Load(0)) return;
    const FroxelGrid g = froxelGrid(P[0].x);
    const uint index = queue.Load(16 + item * 4);
    const uint tileCount = g.gridX * g.gridY, s = index / tileCount, t = index % tileCount;
    const uint2 tile = uint2(t % g.gridX, t / g.gridX);
    RWByteAddressBuffer air = ResourceDescriptorHeap[P[4].w];
    // (P[6].x: bit 0 the fog is on - Fog.hlsli; bit 1 FROXEL_CLIP_AT_SURFACE)
    froxelStoreAir(air, index, froxelAirSlice(g, tile, s, (P[6].x & 1u) != 0, (P[6].x & FROXEL_CLIP_AT_SURFACE) != 0));
}
