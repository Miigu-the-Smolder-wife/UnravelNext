// unx-kernel: cs_6_6 main
// r.gi.ltv.mark (LumenTranslucencyVolume.hlsli): every cell the view sees into marks the radiance cache's probes around
// its sample point (LumenRadianceCacheMark.hlsli), between the cache's clear and its update, at a clipmap P[0].w levels
// coarser than the point's own (Unreal's ShareRadianceCacheWithOpaque.ClipmapBias: the volume fills the frustum, the
// opaque gather only its surfaces - at the surfaces' probe density the volume alone would take the cache's budget).
// P[0] = { indirection UAV (Texture3D R32_UINT), radiance cache params SRV (raw), depth pyramid SRV, clipmap bias }
// P[4], P[8]: LumenTranslucencyVolumeGrid.hlsli
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"
#include "Passes/GI/LumenRadianceCacheMark.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= ltvGridSize()) || !ltvCellVisible(id, P[0].z)) return;
    const LrcParams p = lrcParams(P[0].y);
    const float3 position = ltvCellPosition(float3(id) + ltvFrameJitter());
    const uint own = lrcClipmap(p, position, 0.5);
    if (own >= p.clipmaps) return;
    const uint clipmap = min(own + P[0].w, p.clipmaps - 1);
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].x];
    const int3 corner = int3(floor(lrcCoordFloat(p, position, clipmap) - 0.5));
    for (uint i = 0; i < 8; ++i)
    {
        const int3 c = corner + int3(i & 1, (i >> 1) & 1, i >> 2);
        if (any(c < 0) || any(c >= int(p.grid))) continue;
        indirection[uint3(c.x + int(clipmap * p.grid), c.y, c.z)] = LRC_USED;
    }
}
