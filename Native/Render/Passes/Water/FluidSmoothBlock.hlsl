// unx-kernel: cs_6_6 main
// All three seven-tap axes in one dispatch. A block is processed in two z slabs
// with a three-node halo: 14*14*10 int4 = 31,360 bytes of group memory. Each
// intermediate retains the old signed fixed-point rounding and the old sparse
// domain's zero boundary. Only the final field is published to device memory.
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

groupshared int4 nodes[14 * 14 * 10];

uint smoothSlot(RWByteAddressBuffer table, int3 node)
{
    const int3 block = node >> 3;
    return fsInside(block) ? table.Load(4 * fsBlockIndex(block)) : 0u;
}

int4 smoothSource(RWByteAddressBuffer table, RWByteAddressBuffer source, int3 node)
{
    const uint slot = smoothSlot(table, node);
    if (!slot) return 0;
    const uint3 local = uint3(node & 7);
    return asint(source.Load4(16 * ((slot - 1) * FS_BLOCK_NODES + (local.z * 8 + local.y) * 8 + local.x)));
}

uint smoothIndex(uint3 p) { return (p.z * 14 + p.y) * 14 + p.x; }

int4 smoothAxis(uint at, uint stride)
{
    int4 sum = 44 * nodes[at];
    sum += 15 * (nodes[at - stride] + nodes[at + stride]);
    sum -= 6 * (nodes[at - 2 * stride] + nodes[at + 2 * stride]);
    sum += nodes[at - 3 * stride] + nodes[at + 3 * stride];
    return (sum + 32) >> 6;
}

[numthreads(512, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint block = group.y * WATER_LINEAR_ROW + group.x;
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (block >= min(counters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks())) return;
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer source = ResourceDescriptorHeap[P[8].z];
    RWByteAddressBuffer destination = ResourceDescriptorHeap[P[8].w & 0x3FFFFFFFu];
    const int3 base = fsBlockCoord(scan.Load(4 * (FS_SLOTS(fsTableSize()) + block))) * 8;
    [unroll] for (uint slab = 0; slab < 2; ++slab)
    {
        const int3 origin = base + int3(-3, -3, int(slab * 4) - 3);
        for (uint i = lane; i < 14 * 14 * 10; i += 512)
        {
            const int3 p = int3(i % 14, (i / 14) % 14, i / (14 * 14));
            nodes[i] = smoothSource(table, source, origin + p);
        }
        GroupMemoryBarrierWithGroupSync();

        // Compute before overwriting any source cell. x's outputs extend in y
        // and z for the later axes, but missing sparse blocks must remain zero.
        int4 result[3];
        [unroll] for (uint k = 0; k < 3; ++k)
        {
            const uint i = lane + 512 * k;
            if (i >= 8 * 14 * 10) continue;
            const uint3 p = uint3(i % 8 + 3, (i / 8) % 14, i / (8 * 14));
            result[k] = smoothSlot(table, origin + int3(p)) ? smoothAxis(smoothIndex(p), 1) : int4(0, 0, 0, 0);
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint k = 0; k < 3; ++k)
        {
            const uint i = lane + 512 * k;
            if (i < 8 * 14 * 10)
                nodes[smoothIndex(uint3(i % 8 + 3, (i / 8) % 14, i / (8 * 14)))] = result[k];
        }
        GroupMemoryBarrierWithGroupSync();

        [unroll] for (uint k = 0; k < 2; ++k)
        {
            const uint i = lane + 512 * k;
            if (i >= 8 * 8 * 10) continue;
            const uint3 p = uint3(i % 8 + 3, (i / 8) % 8 + 3, i / 64);
            result[k] = smoothSlot(table, origin + int3(p)) ? smoothAxis(smoothIndex(p), 14) : int4(0, 0, 0, 0);
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint k = 0; k < 2; ++k)
        {
            const uint i = lane + 512 * k;
            if (i < 8 * 8 * 10)
                nodes[smoothIndex(uint3(i % 8 + 3, (i / 8) % 8 + 3, i / 64))] = result[k];
        }
        GroupMemoryBarrierWithGroupSync();

        if (lane < 256)
        {
            const uint3 p = uint3(lane % 8 + 3, (lane / 8) % 8 + 3, lane / 64 + 3);
            int4 value = smoothAxis(smoothIndex(p), 14 * 14);
            value.x = max(value.x, 0);
            destination.Store4(16 * (block * FS_BLOCK_NODES + slab * 256 + lane), asuint(value));
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
