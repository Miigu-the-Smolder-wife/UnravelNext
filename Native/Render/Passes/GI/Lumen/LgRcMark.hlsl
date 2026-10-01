// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.rcmark (with lumen.radiance_cache): every screen probe (uniform and adaptive) marks the 8 radiance
// cache cells around its position (LumenRadianceCacheMark.hlsli), between the cache's clear and its update, so the
// probes its rays will read exist this frame. The clipmap dither is the one LgTrace and LgLightingPdf use for the same
// probe (lgRcDither).
// P[1] = { indirection UAV (Texture3D R32_UINT), radiance cache params SRV (raw), 0, 0 }, P[10].z adaptive SRV,
// P[10].w / P[11].y probe depth / position SRVs.
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"
#include "Passes/GI/LumenRadianceCacheMark.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 atlas = id.xy;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive) || !(probeDepth[atlas] > 0)) return;
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[1].x];
    lrcMark(indirection, lrcParams(P[1].y), probePosition[atlas].xyz, lgRcDither(atlas));
}
