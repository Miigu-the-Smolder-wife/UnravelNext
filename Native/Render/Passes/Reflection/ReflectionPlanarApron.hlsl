// unx-kernel: cs_6_6 main
// Planar view mask apron (INTERFACES v1.28, M request 20260925_M_planar_mask_apron): after the classification wrote a
// view's mask (1 = mirror pixel, 0 = not), every 0 pixel with a mirror pixel in its 3 x 3 neighbourhood becomes 2 (drawn
// and shaded by the view, never read by R: the mirror edge's neighbourhood holds reflected geometry instead of sky), and
// the tile mask is rewritten from the dilated mask (nonzero = the 8 x 8 tile has a pixel to draw). In place: only 0 -> 2
// writes happen and only 1 is tested, so reading neighbours other groups write is race-free. One group per 8 x 8 tile.
// P[0] = { mask UAV (R8_UINT), tile mask UAV (R8_UINT), view width, view height }
#include "Bindless.hlsli"

groupshared uint g_any;

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    RWTexture2D<uint> mask = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> tiles = ResourceDescriptorHeap[P[0].y];
    const int2 size = int2(P[0].zw);
    if (lane == 0) g_any = 0;
    GroupMemoryBarrierWithGroupSync();
    const int2 p = int2(tile * 8 + local);
    if (all(p < size))
    {
        uint value = mask[p];
        if (value == 0)
        {
            bool near = false;
            [unroll] for (int k = 0; k < 9; ++k)
            {
                const int2 q = p + int2(k % 3 - 1, k / 3 - 1);
                if (k != 4 && all(q >= 0) && all(q < size) && mask[q] == 1) near = true;
            }
            if (near)
            {
                value = 2;
                mask[p] = 2;
            }
        }
        if (value != 0) g_any = 1;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) tiles[tile] = g_any;
}
