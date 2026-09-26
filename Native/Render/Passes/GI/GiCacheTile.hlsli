// GI cache lookups of an 8 x 8 pixel group resolved once per group (M's per-pixel irradiance, ScreenProbes pad1). The
// per-pixel lookup (giCacheIrradianceScreen) finds each of its 8 cells per level through the hash table, then the
// entry's update count and anchor, then its 16 map texels: dependent memory round trips per cell, paid inside M's
// low-occupancy shading kernel, while the group's 64 pixels read 38 distinct cells on average [measured, city 4K,
// GiGate --lookup-stats, before the partner corners]. Here the group first files the cells its pixels read at their
// own level (and the next coarser one in the band) into a groupshared table (CAS on a 32-bit tag, the full key beside
// it), resolves every distinct cell once with one lane each (hash probe, update count, anchor normal: those round trips
// once per group, in parallel), and each pixel then evaluates the same level loop with the entries and anchors from
// groupshared; only the map texels come from memory, all independent. A cell that is not in the table (a fill level
// beyond own + 1, a table overflow, two keys with one tag) takes the per-pixel path (giScreenCell). Same entries and the
// same level function (giScreenLevel) as giCacheIrradianceScreen: the same result.
// Use: #define GI_CACHE_TILE before including; every lane of the group calls giCacheTileFile (valid or not), then
// GroupMemoryBarrierWithGroupSync, giCacheTileResolve, the barrier again, then giCacheIrradianceTile per pixel.
#ifndef UNX_GI_CACHETILE_HLSLI
#define UNX_GI_CACHETILE_HLSLI
#include "Passes/GI/GiCache.hlsli"

#define GI_TILE_SLOTS 256u  // groupshared table (a tile files <= 27 cells per level and normal class on a plane)
#define GI_TILE_EMPTY 0u
groupshared uint gs_giTileTag[GI_TILE_SLOTS];      // giHash(key) | 1, 0 = empty
groupshared uint2 gs_giTileKey[GI_TILE_SLOTS];     // the full key (written by the lane that claimed the slot)
groupshared uint2 gs_giTileEntry[GI_TILE_SLOTS];   // { entry or GI_ENTRY_PENDING (none, or no update yet), anchor normal (packed) }

void giCacheTileClear(uint lane)
{
    for (uint i = lane; i < GI_TILE_SLOTS; i += 64) gs_giTileTag[i] = GI_TILE_EMPTY;
}

// Files one cell key (claims a slot, or finds its tag there already).
void giCacheTileInsert(uint64_t key)
{
    const uint tag = giHash(key) | 1u;
    uint slot = tag & (GI_TILE_SLOTS - 1);
    [loop] for (uint i = 0; i < 16; ++i)
    {
        uint prev;
        InterlockedCompareExchange(gs_giTileTag[slot], GI_TILE_EMPTY, tag, prev);
        if (prev == GI_TILE_EMPTY)
        {
            gs_giTileKey[slot] = uint2((uint)key, (uint)(key >> 32));
            return;
        }
        if (prev == tag) return;  // this key, or another with its tag (that one then takes the per-pixel path)
        slot = (slot + 1) & (GI_TILE_SLOTS - 1);
    }
}

// Files the cells a pixel's lookup reads first: its level's 8 cells, and the next coarser level's in the band.
void giCacheTileFile(GiHeader h, bool valid, float3 worldPos, float3 normal)
{
    if (!valid) return;
    uint level;
    const float beta = giLevelBand(h, worldPos, level);
    const uint nc = giNormalClass(normal);
    [unroll] for (uint k = 0; k < 2; ++k)
    {
        if (level + k > h.maxLevel || (k == 1 && beta <= 0)) break;
        const float s = giCellSize(h, level + k);
        const int3 c0 = int3(floor(worldPos / s - 0.5));
        [unroll] for (uint c = 0; c < 8; ++c) giCacheTileInsert(giKey(level + k, nc, c0 + int3(c & 1, (c >> 1) & 1, c >> 2)));
    }
}

// One lane per filed cell (4 slots per lane): the hash probe, the update count and the anchor normal.
template <typename B>
void giCacheTileResolve(B b, GiHeader h, uint lane)
{
    [unroll] for (uint j = 0; j < GI_TILE_SLOTS / 64; ++j)
    {
        const uint slot = lane + 64 * j;
        if (gs_giTileTag[slot] == GI_TILE_EMPTY) continue;
        const uint2 k = gs_giTileKey[slot];
        uint anchor;
        const uint entry = giScreenCell(b, h, (uint64_t)k.x | ((uint64_t)k.y << 32), anchor);
        gs_giTileEntry[slot] = uint2(entry, anchor);
    }
}

// The cell's entry and packed anchor normal: from the group's table, else through the hash table (giScreenCell).
template <typename B>
uint giCacheTileEntry(B b, GiHeader h, uint64_t key, out uint anchor)
{
    const uint tag = giHash(key) | 1u;
    uint slot = tag & (GI_TILE_SLOTS - 1);
    [loop] for (uint i = 0; i < 16; ++i)
    {
        const uint t = gs_giTileTag[slot];
        if (t == GI_TILE_EMPTY) break;
        if (t == tag && all(gs_giTileKey[slot] == uint2((uint)key, (uint)(key >> 32))))
        {
            const uint2 e = gs_giTileEntry[slot];
            anchor = e.y;
            return e.x;
        }
        slot = (slot + 1) & (GI_TILE_SLOTS - 1);
    }
    return giScreenCell(b, h, key, anchor);
}

// giCacheIrradianceScreen with the cells resolved by the group (same result).
template <typename B>
float3 giCacheIrradianceTile(B b, GiHeader h, float3 worldPos, float3 normal, out float weight)
{
    uint level;
    const float beta = giLevelBand(h, worldPos, level);
    const uint nc = giNormalClass(normal);
    float3 result = 0;
    float remain = 1;
    [loop] for (uint k = 0; k < GI_FILL_LEVELS && remain > 1e-3 && level <= h.maxLevel; ++k, ++level)
    {
        const float3 f = worldPos / giCellSize(h, level) - 0.5;
        const int3 c0 = int3(floor(f));
        uint4 entryLo, entryHi, anchorLo, anchorHi;
        uint has = 0;
        [unroll] for (uint c = 0; c < 8; ++c)
        {
            uint anchor;
            const uint entry = giCacheTileEntry(b, h, giKey(level, nc, c0 + int3(c & 1, (c >> 1) & 1, c >> 2)), anchor);
            if (c < 4) entryLo[c] = entry, anchorLo[c] = anchor;
            else entryHi[c - 4] = entry, anchorHi[c - 4] = anchor;
            if (entry != GI_ENTRY_PENDING) has |= 1u << c;
        }
        float3 s;
        float w;
        giScreenLevel(b, h, entryLo, entryHi, anchorLo, anchorHi, has, f - floor(f), normal, s, w);
        const float take = k == 0 ? 1 - beta : 1.0;
        result += (remain * take) * s;
        remain *= 1 - take * w;
    }
    weight = 1 - remain;
    return weight > 0 ? result / weight : 0;
}
#endif
