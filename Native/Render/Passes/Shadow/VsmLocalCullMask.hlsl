// unx-kernel: cs_6_6 main
// Tile masks of the local-light raster views (V's cull mask, INTERFACES 5.3): view v = (active light a, face, mip) with
// v = a x 42 + face x 7 + mip, 128 x 128 tiles of 128 px over the shared 16384^2 viewport (512 words per view); a bit
// is set where the tile is a page of the view's mip that is resident and dirty (rendered this frame).
// P[0].x page table SRV (raw), P[0].y local mask UAV (raw), P[0].z view count, P[0].w active slots SRV
// (StructuredBuffer<uint>: active light a -> shadow slot)
#include "Bindless.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const uint view = id / 512, word = id % 512;
    if (view >= P[0].z) return;
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer mask = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<uint> active = ResourceDescriptorHeap[P[0].w];
    const uint light = active[view / 42], face = (view % 42) / 7, mip = view % 7;
    const uint n = 1u << mip, y = word / 4, x0 = (word % 4) * 32;
    uint bits = 0;
    if (y < n)
        [loop] for (uint b = 0; b < 32 && x0 + b < n; ++b)
        {
            const uint e = table.Load(vsmLocalSlot(light, face, mip, uint2(x0 + b, y)) * 8);
            if ((e & (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) == (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) bits |= 1u << b;
        }
    mask.Store(id * 4, bits);
}
