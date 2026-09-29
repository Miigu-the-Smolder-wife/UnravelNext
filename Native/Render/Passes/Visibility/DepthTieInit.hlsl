// unx-kernel: cs_6_6 main
// Clear one 64-bit winner per pixel; build full-list replay arguments without
// changing the two-phase cull arguments. P[0]: winners, args, state, capacity;
// P[1]: width, height, unused, unused.
#include "Passes/Visibility/VisibilityCommon.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    RWByteAddressBuffer winners = ResourceDescriptorHeap[P[0].x];
    if (all(pixel < P[1].xy)) winners.Store2((pixel.y * P[1].x + pixel.x) * 8, 0xFFFFFFFFu);
    if (any(pixel != 0)) return;
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint list = 0; list < VS_LISTS; ++list)
    {
        const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), P[0].w);
        args.Store3(list * 12, uint3(min(count, 65535u), (count + 65534u) / 65535u, 1));
    }
}
