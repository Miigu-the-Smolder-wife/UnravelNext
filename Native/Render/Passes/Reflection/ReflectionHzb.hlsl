// unx-kernel: cs_6_6 main
// One level of the screen traces' depth pyramid (ScreenTrace.hlsli), one thread per texel of the level: the closest
// depth (the largest device depth) of its 2 x 2 texels of the level below - the depth buffer for level 1, the atlas
// (read and written in one pass: different regions) above it.
// P[0] = { depth SRV, atlas UAV (R32F), level (1..SCT_LEVELS), 0 }, P[1] = { view width, height, 0, 0 }.
#include "Passes/Reflection/ScreenTrace.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint level = P[0].z;
    const uint2 size = P[1].xy;
    const uint2 own = sctLevelSize(size, level);
    if (any(id.xy >= own)) return;
    RWTexture2D<float> atlas = ResourceDescriptorHeap[P[0].y];
    float closest = 0;
    if (level == 1)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
        for (uint k = 0; k < 4; ++k) closest = max(closest, depth.Load(int3(min(id.xy * 2 + uint2(k & 1, k >> 1), size - 1), 0)));
    }
    else
    {
        const uint2 below = sctLevelSize(size, level - 1), origin = sctLevelOrigin(size, level - 1);
        for (uint k = 0; k < 4; ++k) closest = max(closest, atlas[origin + min(id.xy * 2 + uint2(k & 1, k >> 1), below - 1)]);
    }
    atlas[sctLevelOrigin(size, level) + id.xy] = closest;
}
