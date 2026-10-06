// unx-kernel: cs_6_6 main
// Fluid surface: one axis of the node field's low-pass (one group per active block, indirect argument 1; one thread per
// node). The particles' B-spline density carries their sampling noise - the level set of randomly placed particles is
// bumpy at the particle spacing h, which is the node spacing (a millimetre-high bump every 2-3 cm on the D0 fluid:
// glints and chaotic caustics that are no part of the water's shape). The filter is the 7-tap maximally flat low-pass
// [1, -6, 15, 44, 15, -6, 1] / 64 per axis (x, y, z in three passes: separable): gain exactly 1 at zero frequency with
// its 2nd and 4th derivatives 0 (no shrinkage of curved surfaces to 4th order in h / radius), exactly 0 at wavelength
// 2h; 0.997 at 8h, 0.875 at 4h, 0.58 at 3h. A flat face stays where it was (the density across it is point-symmetric
// about its 0.5 crossing and the filter is symmetric), mass is kept (the taps sum to 1). Integer arithmetic on the fixed-
// point sums (floor division by 64 after adding 32): deterministic. All four words (density, weighted velocity) are
// filtered alike, so the surface velocity stays the density-weighted mean. Nodes outside the active blocks read 0 (the
// unfiltered value there) and are not written: the 0.5 level lies more than the filter's reach inside the active blocks.
// P[8].z source UAV, P[8].w destination UAV | axis << 30; the last pass (z) clamps the density at 0 (the level test
// reads it unsigned; the negative lobes lie far below 0.5).
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

// One 8x8 slab, extended by three nodes on each end of the filter axis.
// Every source/table lookup serves up to seven outputs without changing the
// fixed-point filter or its order of operations.
groupshared int4 gs_nodes[14 * 8 * 8];

int4 smoothNode(RWByteAddressBuffer table, RWByteAddressBuffer source, int3 j)
{
    const int3 b = j >> 3;
    if (!fsInside(b)) return 0;
    const uint slot = table.Load(4 * fsBlockIndex(b));
    if (slot == 0) return 0;
    const int3 l = j - b * 8;
    return asint(source.Load4(16 * ((slot - 1) * FS_BLOCK_NODES + ((uint)l.z * 8 + (uint)l.y) * 8 + (uint)l.x)));
}

[numthreads(512, 1, 1)]
void main(uint t : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint g = group.y * WATER_LINEAR_ROW + group.x;  // the active block (rows of WATER_LINEAR_ROW groups)
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (g >= min(counters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks())) return;
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer source = ResourceDescriptorHeap[P[8].z];
    RWByteAddressBuffer destination = ResourceDescriptorHeap[P[8].w & 0x3FFFFFFFu];
    const uint axis = P[8].w >> 30;
    const int3 block = fsBlockCoord(scan.Load(4 * (FS_SLOTS(fsTableSize()) + g))) * 8;
    for (uint i = t; i < 14u * 8u * 8u; i += 512u)
    {
        const int along = int(i % 14u) - 3, a = int((i / 14u) % 8u), b = int(i / (14u * 8u));
        const int3 offset = axis == 0 ? int3(along, a, b) : axis == 1 ? int3(a, along, b) : int3(a, b, along);
        gs_nodes[i] = smoothNode(table, source, block + offset);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint3 local = uint3(t % 8, (t / 8) % 8, t / 64);
    const uint a = axis == 0 ? local.y : local.x, b = axis == 2 ? local.y : local.z;
    const uint centre = (b * 8u + a) * 14u + local[axis] + 3u;
    int4 sum = 44 * gs_nodes[centre];
    sum += 15 * (gs_nodes[centre - 1] + gs_nodes[centre + 1]);
    sum -= 6 * (gs_nodes[centre - 2] + gs_nodes[centre + 2]);
    sum += gs_nodes[centre - 3] + gs_nodes[centre + 3];
    int4 r = (sum + 32) >> 6;  // arithmetic shift: floor((sum + 32) / 64) for either sign
    if (axis == 2) r.x = max(r.x, 0);
    destination.Store4(16 * (g * FS_BLOCK_NODES + t), asuint(r));
}
