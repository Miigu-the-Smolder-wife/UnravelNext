// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.adaptive.mark: one thread per tile. Each of the tile's candidate pixels (LgAdaptive.hlsli) whose 4
// uniform probes together weigh less than the minimum interpolation weight is marked: bit i of the tile's mask.
// P[0] = LgSurface inputs, P[1].x = mask UAV (R32_UINT, probe view size). P[10].w / P[11].y = probe depth / position
// SRVs (the uniform probes). b1 = the view.
#include "Passes/GI/Lumen/LgAdaptive.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= lgProbeViewSize())) return;
    RWTexture2D<uint> mask = ResourceDescriptorHeap[P[1].x];
    uint bits = 0;
    [loop] for (uint i = 0; i < LG_TILE_ADAPTIVE; ++i)
    {
        const uint2 pixel = lgAdaptivePixel(id.xy, i);
        if (any(pixel >= lgViewSize())) continue;
        const LgSurface s = lgSurface(pixel);
        if (!s.valid) continue;
        LgProbeSample ps;
        lgProbeWeights((float2)pixel, 0, s.position, s.depth, s.normal, false, false, ps);
        if (dot(ps.weights, 1) < LG_MIN_INTERPOLATION_WEIGHT) bits |= 1u << i;
    }
    mask[id.xy] = bits;
}
