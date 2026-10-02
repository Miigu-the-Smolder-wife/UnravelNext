// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.adaptive.mark: one thread per tile. Each of the tile's candidate pixels (LgAdaptive.hlsli) whose 4
// uniform probes together weigh less than the minimum interpolation weight is marked: bit i of the tile's mask.
// A foliage pixel (a Foliage or Subsurface material: the reference's ScreenProbeMaterial.bHasBackfaceDiffuse) is weighed
// with the looser plane weight, as the integration weighs it - else leaves ask for adaptive probes the integration does
// not need.
// P[0] = LgSurface inputs, P[1].x = mask UAV (R32_UINT, probe view size), P[1].y = M's material word SRV (0xFFFFFFFF:
// the scene has neither Foliage nor Subsurface). P[10].w / P[11].y = probe depth / position SRVs (the uniform probes).
// b1 = the view.
#include "Passes/GI/Lumen/LgAdaptive.hlsli"
#include "Scene.hlsli"

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
        bool foliage = false;
        if (P[1].y != 0xFFFFFFFFu)
        {
            Texture2D<uint> words = ResourceDescriptorHeap[P[1].y];
            const uint cls = materialClass(loadMaterial(words.Load(int3(pixel, 0)) & 0xFFFFu));
            foliage = cls == MATERIAL_FOLIAGE || cls == MATERIAL_SUBSURFACE;
        }
        LgProbeSample ps;
        lgProbeWeights((float2)pixel, 0, s.position, s.depth, s.normal, false, foliage, ps);
        if (dot(ps.weights, 1) < LG_MIN_INTERPOLATION_WEIGHT) bits |= 1u << i;
    }
    mask[id.xy] = bits;
}
