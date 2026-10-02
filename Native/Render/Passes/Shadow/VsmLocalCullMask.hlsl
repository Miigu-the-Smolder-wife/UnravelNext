// unx-kernel: cs_6_6 main
// Tile masks and atlas slots of the local-light raster views (V's cull mask and tile atlas, INTERFACES 5.3): view =
// (active light a, face, mip) with a viewport of res = 128 x 2^mip texels, 2^mip x 2^mip tiles of 128 px. The views'
// masks are packed (VsmLocal.hlsli VSM_LOCAL_*_WORDS): mip after mip, face after face, light after light. A bit is set
// where the tile is a page of that mip with a physical page this frame; its atlas slot goes to word (mask word) x 32 +
// bit of the slots buffer. Only pages given this frame (DIRTY): a page kept from an earlier frame (page cache, VsmCache.hlsl
// MODE 3) holds its content and is not drawn again, as the sun's (VsmCullMask). One thread per mask word.
// shadow.vsm.local_static_separate (P[1].y = 1): a second set of mask words after the first (word VSM_LOCAL_LIGHTS x
// VSM_LOCAL_LIGHT_WORDS on) for the movable casters' views - the pages drawn anew and the kept pages whose movable
// casters are drawn anew (VSM_FLAG_DIRTY_DYNAMIC); the first set is the static casters' (the pages drawn anew). The slots
// are one set: a page has the same place in both atlases (the movable casters' views name them by
// RasterView::atlasSlotOffset).
// P[0].x page table SRV (raw), P[0].y local mask UAV (raw), P[0].z active light count, P[0].w active slots SRV
// (StructuredBuffer<uint>: active light a -> shadow slot); P[1].x local atlas slots UAV (raw)
#include "Bindless.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const uint a = id / VSM_LOCAL_LIGHT_WORDS, w = id % VSM_LOCAL_LIGHT_WORDS;
    if (a >= P[0].z) return;
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer mask = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[1].x];
    StructuredBuffer<uint> active = ResourceDescriptorHeap[P[0].w];
    const uint light = active[a], face = w / VSM_LOCAL_FACE_WORDS, r = w % VSM_LOCAL_FACE_WORDS;
    uint mip = 0;
    [unroll] for (uint m = 1; m < VSM_LOCAL_MIPS; ++m)
        if (r >= vsmLocalViewWordOffset(m)) mip = m;
    const uint n = 1u << mip, wordInView = r - vsmLocalViewWordOffset(mip);
    const bool separate = P[1].y != 0;
    uint bits = 0, bitsDynamic = 0;
    [loop] for (uint b = 0; b < 32; ++b)
    {
        const uint tile = wordInView * 32 + b;
        if (tile >= n * n) break;
        const uint e = table.Load(vsmLocalSlot(light, face, mip, uint2(tile % n, tile / n)) * 8);
        const bool resident = (e & VSM_FLAG_RESIDENT) != 0;
        if (resident && (e & VSM_FLAG_DIRTY) != 0) bits |= 1u << b;
        if (separate && resident && (e & (VSM_FLAG_DIRTY | VSM_FLAG_DIRTY_DYNAMIC)) != 0) bitsDynamic |= 1u << b;
        if (((bits | bitsDynamic) >> b) & 1u) slots.Store((id * 32 + b) * 4, e & VSM_PHYS_MASK);
    }
    mask.Store(id * 4, bits);
    if (separate) mask.Store((VSM_LOCAL_LIGHTS * VSM_LOCAL_LIGHT_WORDS + id) * 4, bitsDynamic);
}
