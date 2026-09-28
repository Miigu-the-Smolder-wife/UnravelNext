// unx-kernel: cs_6_6 main
// Clears the cull state words and indirect arguments of a cull run (CullShared.hlsli root layout); the phase 1 dispatch
// over the GPU-written instances (VA_GPU_INSTANCES) gets ceil(live / 64) x views groups from their live count (the
// direct instance dispatch covers the CPU instances only: sized by the capacity it launched 65536 / 64 x views groups
// per run, e.g. 258 K per local-light raster request of 252 views, almost all without an instance).
#include "Passes/Visibility/CullShared.hlsli"

[numthreads(128, 1, 1)]  // VS_WORDS, VA_WORDS <= 128
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer args = ResourceDescriptorHeap[ARGS_UAV];
    if (i < VS_WORDS) state.Store(4 * i, 0);
    if (i < VA_WORDS)
    {
        uint v = 0;
        if (i >= VA_GPU_INSTANCES && i < VA_GPU_INSTANCES + 3 && VIEW_COUNT > 0)
        {
            const uint live = gpuInstanceCount(loadView(0));
            const uint groupsX = (live + 63) / 64;
            v = i == VA_GPU_INSTANCES ? groupsX : groupsX == 0 ? 0 : i == VA_GPU_INSTANCES + 1 ? VIEW_COUNT : 1u;
        }
        args.Store(4 * i, v);
    }
}
