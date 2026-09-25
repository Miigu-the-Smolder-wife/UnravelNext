// unx-kernel: cs_6_6 main
// Planar reflection test only (PlanarMirror.cpp, run B): the view's mirror mask against the classification. One group
// per 8 x 8 tile of the view. A view texel p is main pixel origin + p; it must be 1 exactly when that pixel's tile was
// written by R (validity texel) and its mode is REFL_PLANAR of view 0. A tile mask texel must be the OR of its texels.
// P[0] = { mask SRV, tile mask SRV, modes SRV, reflection SRV }, P[1] = { origin x, origin y, view width, view height },
// P[2].x = result UAV (raw: pixel errors, tile errors, mirror texels, view texels)
#include "Passes/Reflection/ReflectionInternal.hlsli"

groupshared uint g_any;

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    Texture2D<uint> mask = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> tiles = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> reflection = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[2].x];
    if (lane == 0) g_any = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = tile * 8 + local;
    if (all(p < P[1].zw))
    {
        const uint2 pixel = P[1].xy + p;
        uint width, height;
        reflection.GetDimensions(width, height);
        const bool written = reflection.Load(int3(pixel.x / 8, reflectionPixelRows(height) + pixel.y / 8, 0)).a > 0.5;
        const uint m = modes.Load(int3(pixel, 0));
        const uint expected = written && reflMode(m) == REFL_PLANAR && ((m >> 2) & 7u) == 0 ? 1u : 0u;
        const uint got = mask.Load(int3(p, 0));
        if (got != 0) InterlockedOr(g_any, 1u);
        uint previous;
        if ((got != 0 ? 1u : 0u) != expected) result.InterlockedAdd(0, 1u, previous);
        if (got != 0) result.InterlockedAdd(8, 1u, previous);
        result.InterlockedAdd(12, 1u, previous);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && (tiles.Load(int3(tile, 0)) != 0 ? 1u : 0u) != g_any)
    {
        uint previous;
        result.InterlockedAdd(4, 1u, previous);
    }
}
