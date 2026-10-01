// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.place: the uniform screen probes (LgCommon.hlsli). One thread per tile: the tile's probe pixel of
// this frame, its surface into the probe textures (depth < 0: no surface there).
// P[0] = LgSurface inputs. P[1] = { probe depth UAV (R32F), probe normal UAV (RG16 unorm), probe position UAV (RGBA32F:
// world position, world speed in m per frame), 0 }, P[10].z = adaptive buffer UAV (cleared here). b1 = the view.
#include "Passes/GI/Lumen/LgSurface.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= lgProbeViewSize())) return;
    RWTexture2D<float> probeDepth = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float2> probeNormal = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> probePosition = ResourceDescriptorHeap[P[1].z];
    // (this frame's adaptive probes start empty: the count and the tile's list header)
    RWByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    if (all(id.xy == 0)) adaptive.Store(0, 0u);
    adaptive.Store(lgTileHeaderAddress(id.xy), 0u);
    const uint2 pixel = lgUniformProbePixel(id.xy);
    const LgSurface s = lgSurface(pixel);
    if (!s.valid)
    {
        probeDepth[id.xy] = -1;
        probeNormal[id.xy] = float2(0.5, 0.5);
        probePosition[id.xy] = 0;
        return;
    }
    probeDepth[id.xy] = s.depth;
    probeNormal[id.xy] = lgEncodeNormal(s.normal);
    probePosition[id.xy] = float4(s.position, distance(s.position, lgPreviousPosition(pixel, s)));
}
