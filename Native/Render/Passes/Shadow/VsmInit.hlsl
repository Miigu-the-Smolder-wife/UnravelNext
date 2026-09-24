// unx-kernel: cs_6_6 main
// Resets the VSM state when the pool is (re)created: empty page table, no requests, every physical page free.
// P[0].x page table UAV (raw, 8 B per slot), P[0].y requests UAV (raw), P[0].z page metadata UAV (uint4 per page),
// P[0].w free list UAV (raw: count, then page indices); P[1].x slots, P[1].y physical pages
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint4> meta = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[0].w];
    const uint slots = P[1].x, pages = P[1].y;
    if (i < slots)
    {
        table.Store2(i * 8, uint2(0, 0));
        requests.Store(i * 4, 0);
    }
    if (i < pages)
    {
        meta[i] = uint4(0, 0, 0, 0);
        freeList.Store(4 + i * 4, pages - 1 - i);  // popped from the end: page 0 first
    }
    if (i == 0) freeList.Store(0, pages);
}
