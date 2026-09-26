// unx-kernel: cs_6_6 main
// Origin rebase (C9, Docs/Design/Requests/20260926_C_origin_rebase.md; FrameContext::originShift = delta, a multiple of
// 1024 m per axis): every world position the renderer holds moves by -delta, so each live entry's cell moves by the
// whole number of cells -delta / s (s = its level's cell size: 0.0625 x 2^level m divides 1024 m up to level 14) and its
// anchor by -delta; the rehash that follows (GiTableClear + GiRehash, every frame) files the entries under their new
// keys. Cells wrap at 18 bits like giKey's, so the moved key is the one the moved position gives. An entry of a coarser
// level whose cell does not divide delta cannot move by whole cells: it is freed like an evicted one (it re-forms from
// its readers' rays; such cells span kilometres). The deterministic anchor candidate is packed relative to the cell, so
// it moves with it unchanged. Runs before GiEvict on the rebase frame only.
// P[0] = { cache UAV, delta x, y, z (float bits) }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity) return;
    const uint4 meta = b.Load4(h.offMeta + entry * 16);
    if (meta.y == 0) return;  // free
    const uint64_t key = (uint64_t)meta.x | ((uint64_t)meta.y << 32);
    const uint level = (uint)(key & 31u);
    const float3 delta = asfloat(P[0].yzw);
    const float3 cells = delta / giCellSize(h, level);
    if (any(cells != round(cells)))
    {
        b.Store2(h.offMeta + entry * 16, uint2(0, 0));
        uint slot;
        b.InterlockedAdd(GI_H_FREE_COUNT, 1u, slot);
        b.Store(h.offFree + slot * 4, entry);
        b.InterlockedAdd(GI_H_STAT_EVICTED, 1u);
        return;
    }
    const int3 move = int3(cells);
    const uint3 cell = uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu;
    const uint3 moved = uint3(int3(cell) - move) & 0x3FFFFu;
    const uint64_t shifted = (key & 0xFFull) | ((uint64_t)moved.x << 8) | ((uint64_t)moved.y << 26) | ((uint64_t)moved.z << 44) | (1ull << 63);
    b.Store2(h.offMeta + entry * 16, uint2((uint)shifted, (uint)(shifted >> 32)));
    const float3 anchor = asfloat(b.Load3(h.offAnchor + entry * 16));
    b.Store3(h.offAnchor + entry * 16, asuint(anchor - delta));
}
