// unx-kernel: cs_6_6 main
// Raster tile mask and atlas slots of the sun levels for V's depth raster service (INTERFACES 5.3, tile atlas v1.32):
// per clipmap level (one raster view each), one bit per 128-texel viewport tile = window page, set when that page is drawn
// this frame (a new physical page, VsmScan; kept pages are not redrawn, VsmCache), and the page's atlas slot in word (level x 512 + word) x 32 + bit of the slots
// buffer. One thread per 32-bit mask word.
// shadow.vsm.static_separate (P[1].x = 1): a second set of mask words and slots after the first (word VSM_LEVELS x 512 on) for
// the movable casters' views - the pages drawn anew and the kept pages whose movable casters are drawn anew
// (VSM_FLAG_DIRTY_DYNAMIC); the first set is the static casters' (the pages drawn anew). A page has the same slot in
// both: the two atlases hold a page at the same place.
// shadow.vsm.static_occlusion_two_phase (P[1].y: the guess UAV, raw; 0xFFFFFFFF: off): for each page drawn anew, the page
// that stands for its occluders in the first cull phase of the static casters' views (V's DepthRasterRequest::tileGuess,
// two words per mask bit of the first set) - the nearest coarser level's page over the same ground that is kept this
// frame (resident, not drawn anew: its static copy and that copy's HZB are those of an earlier frame), up to
// VSM_GUESS_LEVELS levels up, and where the page lies in it: { its physical page (0xFFFFFFFF: none), levels up |
// x << 8 | y << 20 (the page's corner in it, texels) }. The levels' grids nest and share the depth mapping (one basis,
// one caster height range), so a texel block of the coarser page covers the same ground and heights.
// P[0].x page table SRV (raw), P[0].y mask UAV (raw), P[0].z VSM constants CBV, P[0].w atlas slots UAV (raw)
#include "Passes/Shadow/VsmCommon.hlsli"

#define VSM_GUESS_LEVELS 4u

[numthreads(64, 1, 1)]
void main(uint word : SV_DispatchThreadID)
{
    const uint wordsPerLevel = VSM_SLOTS_PER_LEVEL / 32;
    if (word >= VSM_LEVELS * wordsPerLevel) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].w];
    const uint k = word / wordsPerLevel;
    const bool separate = P[1].x != 0;
    const uint second = VSM_LEVELS * wordsPerLevel;  // the movable casters' set of words
    uint bits = 0, bitsDynamic = 0;
    [unroll] for (uint i = 0; i < 32; ++i)
    {
        const uint tile = (word % wordsPerLevel) * 32 + i;  // row-major over the window
        const int2 page = vsmOrigin(c, k) + int2(tile % VSM_TABLE, tile / VSM_TABLE);
        const uint e = table.Load(vsmSlot(page, k) * 8);
        const bool resident = (e & VSM_FLAG_RESIDENT) != 0;
        if (resident && (e & VSM_FLAG_DIRTY) != 0)  // drawn this frame (kept pages: VsmCache)
        {
            bits |= 1u << i;
            slots.Store((word * 32 + i) * 4, e & VSM_PHYS_MASK);
            if (P[1].y != 0xFFFFFFFFu)
            {
                uint2 guess = uint2(0xFFFFFFFFu, 0);
                [unroll] for (uint up = 1; up <= VSM_GUESS_LEVELS; ++up)
                {
                    const uint j = min(k + up, VSM_LEVELS - 1);
                    const int2 ancestor = page >> (int)up;
                    const uint2 a = table.Load2(vsmSlot(ancestor, j) * 8);
                    const bool kept = k + up < VSM_LEVELS && vsmSameBasis(c, k, j) && vsmInWindow(c, ancestor, j) && (a.x & VSM_FLAG_RESIDENT) != 0 &&
                                      (a.x & VSM_FLAG_DIRTY) == 0 && a.y == vsmTag(ancestor);
                    if (kept && guess.x == 0xFFFFFFFFu)
                    {
                        const uint2 corner = uint2(page - (ancestor << (int)up)) * (VSM_PAGE >> up);
                        guess = uint2(a.x & VSM_PHYS_MASK, up | (corner.x << 8) | (corner.y << 20));
                    }
                }
                RWByteAddressBuffer guesses = ResourceDescriptorHeap[P[1].y];
                guesses.Store2((word * 32 + i) * 8, guess);
            }
        }
        if (separate && resident && (e & (VSM_FLAG_DIRTY | VSM_FLAG_DIRTY_DYNAMIC)) != 0)
        {
            bitsDynamic |= 1u << i;
            slots.Store(((second + word) * 32 + i) * 4, e & VSM_PHYS_MASK);
        }
    }
    RWByteAddressBuffer mask = ResourceDescriptorHeap[P[0].y];
    mask.Store(word * 4, bits);
    if (separate) mask.Store((second + word) * 4, bitsDynamic);
}
