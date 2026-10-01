// Marking for the radiance cache (LumenRadianceCache.hlsli): a consumer's kernel marks the 8 probe cells around every
// position it will query this frame, between the cache's clear and its update (gi::lumenRadianceCacheBegin returns the
// indirection texture for the consumer's UAV use). Marks are idempotent (every thread stores the same word).
#ifndef UNX_LUMEN_RADIANCE_CACHE_MARK_HLSLI
#define UNX_LUMEN_RADIANCE_CACHE_MARK_HLSLI
#include "Passes/GI/LumenRadianceCache.hlsli"

void lrcMark(RWTexture3D<uint> indirection, LrcParams p, float3 worldPosition, float dither)
{
    const uint clipmap = lrcClipmap(p, worldPosition, dither);
    if (clipmap >= p.clipmaps) return;
    const int3 corner = int3(floor(lrcCoordFloat(p, worldPosition, clipmap) - 0.5));
    for (uint i = 0; i < 8; ++i)
    {
        const int3 c = corner + int3(i & 1, (i >> 1) & 1, i >> 2);
        if (any(c < 0) || any(c >= int(p.grid))) continue;
        indirection[uint3(c.x + int(clipmap * p.grid), c.y, c.z)] = LRC_USED;
    }
}
#endif
