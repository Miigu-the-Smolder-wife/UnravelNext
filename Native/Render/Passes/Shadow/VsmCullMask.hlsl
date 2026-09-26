// unx-kernel: cs_6_6 main
// Raster tile mask and atlas slots of the sun levels for V's depth raster service (INTERFACES 5.3, tile atlas v1.32):
// per clipmap level (one raster view each), one bit per 128-texel viewport tile = window page, set when that page has a
// physical page this frame (VsmScan), and the page's atlas slot in word (level x 512 + word) x 32 + bit of the slots
// buffer. One thread per 32-bit mask word.
// P[0].x page table SRV (raw), P[0].y mask UAV (raw), P[0].z VSM constants CBV, P[0].w atlas slots UAV (raw)
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint word : SV_DispatchThreadID)
{
    const uint wordsPerLevel = VSM_SLOTS_PER_LEVEL / 32;
    if (word >= VSM_LEVELS * wordsPerLevel) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].w];
    const uint k = word / wordsPerLevel;
    uint bits = 0;
    [unroll] for (uint i = 0; i < 32; ++i)
    {
        const uint tile = (word % wordsPerLevel) * 32 + i;  // row-major over the window
        const int2 page = vsmOrigin(c, k) + int2(tile % VSM_TABLE, tile / VSM_TABLE);
        const uint e = table.Load(vsmSlot(page, k) * 8);
        if (e & VSM_FLAG_RESIDENT)
        {
            bits |= 1u << i;
            slots.Store((word * 32 + i) * 4, e & VSM_PHYS_MASK);
        }
    }
    RWByteAddressBuffer mask = ResourceDescriptorHeap[P[0].y];
    mask.Store(word * 4, bits);
}
