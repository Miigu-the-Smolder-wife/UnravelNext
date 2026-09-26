// unx-kernel: cs_6_6 main
// Resets the VSM state when the atlas is (re)created: empty page table, no requests, empty metadata and no transmittance
// layers (every frame then rewrites the table for the slots it scans, VsmScan).
// P[0].x page table UAV (raw, 8 B per slot), P[0].y requests UAV (raw), P[0].z page metadata UAV (VsmPageMeta),
// P[1].x slots, P[1].y physical pages, P[1].z transmittance layer UAV (raw: its per-page words are cleared: no layer)
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[0].z];
    const uint slots = P[1].x, pages = P[1].y;
    if (i < slots)
    {
        table.Store2(i * 8, uint2(0, 0));
        requests.Store(i * 4, 0);
    }
    if (i < pages)
    {
        meta[i] = (VsmPageMeta)0;
        RWByteAddressBuffer layers = ResourceDescriptorHeap[P[1].z];
        layers.Store(i * 4, 0);
    }
}
