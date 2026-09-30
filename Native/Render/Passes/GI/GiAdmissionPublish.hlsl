// unx-kernel: cs_6_6 main
// P[0]: cache, request capacity, sorted parity, unused. Retain all live entries,
// admit new unique keys in key order into free entry ids in ascending order.
// All publication happens across a dispatch boundary from every reader.
#include "Passes/GI/GiAdmission.hlsli"
[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint i = giAdmissionIndex(group, lane), n = b.Load(h.offAdmission + 8);
    if (i >= n) return;
    const uint live = b.Load(h.offAdmission + 12), free = h.capacity - live;
    const uint source = giAdmissionArray(h, P[0].y, P[0].z);
    const GiAdmissionRecord r = giAdmissionLoad(b, source + i * 32);
    uint rank = r.meta.z, index = i / 256, count = (n + 255) / 256;
    uint offset = giAdmissionScan(h, P[0].y);
    [unroll] for (uint level = 0; level < 4; ++level)
    {
        rank += b.Load(offset + index * 4);
        if (count <= 256) break;
        offset += count * 4; count = (count + 255) / 256; index /= 256;
    }
    if (i == n - 1)
    {
        const uint wanted = rank + r.meta.w, admitted = min(wanted, h.capacity);
        b.Store2(h.offAdmission + 16, uint2(live, admitted - live));
        b.Store(GI_H_FREE_COUNT, h.capacity - admitted);
        b.Store(h.offAdmission, 0u);
        if (wanted > admitted) b.InterlockedAdd(GI_H_STAT_ALLOC_FAIL, wanted - admitted);
    }
    if (r.meta.w == 0 || rank >= h.capacity) return;
    uint entry = r.meta.x;
    if (r.meta.y == 1)
    {
        entry = b.Load(source + (n - free + rank - live) * 32 + 16);
        b.Store4(h.offMeta + entry * 16, uint4(r.keyAnchor.xy, h.frame, 0));
        b.Store4(h.offAnchor + entry * 16, 0u); // decoded by GiDetAnchors before rays
        [unroll] for (uint k = 0; k < GI_SH_STRIDE / 16; ++k) b.Store4(h.offSh + entry * GI_SH_STRIDE + k * 16, 0u);
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT / 2; ++k) b.Store4(h.offTexels + entry * GI_TEXEL_COUNT * 8 + k * 16, 0u);
        [loop] for (uint k = 0; k < GI_IRR_STRIDE / 16; ++k) b.Store4(h.offIrr + entry * GI_IRR_STRIDE + k * 16, 0u);
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT / 4; ++k) b.Store4(giEmitterOffset(h) + entry * GI_TEXEL_COUNT * 4 + k * 16, 0u);
        b.Store2(h.offAnchorMin + entry * 8, r.keyAnchor.zw);
        if (b.Load(GI_RESAMPLE_OFFSET) != 0) b.Store2(b.Load(GI_RESAMPLE_OFFSET) + entry * 8, uint2(0, 0));  // gi.anchor_resample: no stale offer
        // Carried after screen requests so visible entries retain tier 0.
        b.Store(h.offHitStamp + entry * 4, h.frame - 1);
        uint slot;
        const uint parity = (h.frame - 1) & 1u;
        b.InterlockedAdd(GI_H_HIT_COUNT + parity * 4, 1u, slot);
        if (slot < h.capacity) b.Store(h.offHitList + (parity * h.capacity + slot) * 4, entry);
        b.InterlockedAdd(GI_H_STAT_CREATED, 1u);
    }
    b.Store4(h.offTable + rank * 16, uint4(r.keyAnchor.xy, entry, 0));
}
