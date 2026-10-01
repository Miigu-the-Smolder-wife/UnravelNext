// gi.lumen: the candidate pixels of the adaptive probes (LgCommon.hlsli): LG_TILE_ADAPTIVE per tile, a scrambled
// Hammersley set inside the tile (the scramble per tile and frame).
#ifndef UNX_GI_LUMEN_ADAPTIVE_HLSLI
#define UNX_GI_LUMEN_ADAPTIVE_HLSLI
#include "Passes/GI/Lumen/LgInterpolate.hlsli"

uint2 lgAdaptivePixel(uint2 tile, uint index)
{
    const uint2 base = tile * lgTile() + lgTileJitter(lgTemporalIndex());
    const uint seed = lgHash(tile.x * 0x9E3779B1u + tile.y * 0x85EBCA77u + lgTemporalIndex() * 0xC2B2AE3Du);
    const float2 e = lgHammersley(index, LG_TILE_ADAPTIVE, uint2(seed & 0xFFFFu, seed >> 16));
    return base + (uint2)clamp(e * (float)lgTile(), 0.0, (float)lgTile() - 1.0);
}
#endif
