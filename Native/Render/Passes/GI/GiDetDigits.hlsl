// unx-kernel: cs_6_6 main
// Deterministic update selection (gi.deterministic), one radix level: the requested entries in a tier's threshold age
// bucket are ranked by a priority hash of their key and the frame (giDetPriority), not by the order their atomic fills
// arrive in. Level L counts the entries whose priority matches the prefix resolved so far by their next 8 bits.
// P[0] = { cache UAV, selection state UAV (raw, GI_DET_* layout), level 0..3, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const GiHeader h = giHeader(b);
    if (i >= min(h.updateCount, h.capacity)) return;
    const uint item = b.Load(h.offUpdate + i * 4);
    const uint tier = item >> 31, entry = item & ~GI_TIER_HIT;
    const uint2 select = b.Load2(GI_H_SELECT + tier * 16);  // threshold bucket, quota
    if (select.y == 0 || giAgeBucket(b, h, entry) != select.x) return;
    const uint level = P[0].z;
    const uint p = giDetPriority(b, h, entry);
    const uint base = tier * GI_DET_TIER_BYTES;
    if (level > 0)
    {
        const uint mask = 0xFFFFFFFFu << (32 - 8 * level);
        if ((p & mask) != (state.Load(base + GI_DET_PREFIX) & mask)) return;
    }
    state.InterlockedAdd(base + ((p >> (24 - 8 * level)) & 255u) * 4, 1u);
}
