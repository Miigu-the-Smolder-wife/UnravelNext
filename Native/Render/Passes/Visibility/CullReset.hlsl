// unx-kernel: cs_6_6 main
// Clears the cull state words and indirect arguments of a cull run (CullShared.hlsli root layout).
#include "Passes/Visibility/CullShared.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer args = ResourceDescriptorHeap[ARGS_UAV];
    if (i < VS_WORDS) state.Store(4 * i, 0);
    if (i < VA_WORDS) args.Store(4 * i, 0);
}
