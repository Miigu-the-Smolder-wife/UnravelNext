// unx-kernel: cs_6_6 main
// R gate only: the information quantity of M's per-pixel GI cache lookup (ScreenProbes pad1 -> giCacheIrradianceScreen,
// front side), counted on the real frame: per pixel the levels visited, the cells looked up, the hash-table slots read,
// the cells found with data; per 8 x 8 tile the distinct cells (keys) and distinct entries its pixels read. The same
// level loop as giCacheIrradianceScreen (band, fill levels) with the reads counted instead of weighted where the count
// does not depend on the weights. Counters (uint, P[0].w, zeroed by the gate):
//   0 pixels, 1 levels, 2 cell lookups, 3 table slots read, 4 cells with data, 5 tiles with a pixel, 6 sum of distinct
//   keys per tile, 7 sum of distinct entries per tile, 8 max distinct keys, 9 max distinct entries, 10 pixels with more
//   than one level, 11 pixels where the group-resolved lookup (GiCacheTile.hlsli) differs in any bit, 12 its largest
//   relative difference (float bits), 13 cells the tile table did not hold (per-pixel path), 14 entries evaluated
//   (weight > 0 after the partner corners), 16..79 histogram of distinct entries per tile (63 = 63 or more); against the
//   reconstruction before the partner corners (statsFillOnly): 80 largest relative difference (float bits), 81 sum of
//   relative differences x 1e3, 82 pixels compared, 83 over 1 %, 84 over 5 %.
// P[0] = { cache SRV, depth SRV, gbuffer SRV, counters UAV }, P[1] = { width, height, clear, 0 }; b1 = the main view.
// clear = 1: one group zeroes the 80 counters (dispatched first).
#include "GBuffer.hlsli"
#include "Passes/GI/GiCacheTile.hlsli"

#define STATS_SET 1024u
groupshared uint gs_keys[STATS_SET];     // distinct cell keys of the tile (32-bit hashes of the 64-bit keys)
groupshared uint gs_entries[STATS_SET];  // distinct entries of the tile
groupshared uint gs_keyCount, gs_entryCount;

// Inserts v (!= 0) into a groupshared open-addressing set; returns nothing (the count is kept by the caller's counter).
void statsInsert(uint which, uint v)
{
    uint slot = giHash((uint64_t)v | (1ull << 40)) & (STATS_SET - 1);
    [loop] for (uint i = 0; i < STATS_SET; ++i)
    {
        uint prev;
        if (which == 0)
        {
            InterlockedCompareExchange(gs_keys[slot], 0u, v, prev);
            if (prev == 0) { InterlockedAdd(gs_keyCount, 1u); return; }
        }
        else
        {
            InterlockedCompareExchange(gs_entries[slot], 0u, v, prev);
            if (prev == 0) { InterlockedAdd(gs_entryCount, 1u); return; }
        }
        if (prev == v) return;
        slot = (slot + 1) & (STATS_SET - 1);
    }
}

// giFind with the slots it reads counted.
uint statsFind(ByteAddressBuffer b, GiHeader h, uint64_t key, inout uint slots)
{
    uint slot = giHash(key) & (h.tableSlots - 1);
    [loop] for (uint i = 0; i < GI_PROBE_LIMIT; ++i)
    {
        ++slots;
        const uint4 s = b.Load4(h.offTable + slot * 16);
        const uint64_t k = (uint64_t)s.x | ((uint64_t)s.y << 32);
        if (k == key) return s.z;
        if (k == 0) return GI_ENTRY_PENDING;
        slot = (slot + 1) & (h.tableSlots - 1);
    }
    return GI_ENTRY_PENDING;
}

// The screen reconstruction before the partner corners (2026-09-26): each level's trilinear sum over the corners with
// data, the rest of the weight from the next coarser level.
float3 statsFillOnly(ByteAddressBuffer b, GiHeader h, float3 worldPos, float3 normal, out float weight)
{
    uint level;
    const float beta = giLevelBand(h, worldPos, level);
    const uint nc = giNormalClass(normal);
    float3 result = 0;
    float remain = 1;
    [loop] for (uint k = 0; k < GI_FILL_LEVELS && remain > 1e-3 && level <= h.maxLevel; ++k, ++level)
    {
        float3 s = 0, unused = 0;
        float w = 0;
        giAccumulateLevel(b, h, worldPos, normal, normal, false, nc, level, s, unused, w);
        const float take = k == 0 ? 1 - beta : 1.0;
        result += (remain * take) * s;
        remain *= 1 - take * w;
    }
    weight = 1 - remain;
    return weight > 0 ? result / weight : 0;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    if (P[1].z != 0)
    {
        RWByteAddressBuffer zero = ResourceDescriptorHeap[P[0].w];
        zero.Store(lane * 4, 0u);
        zero.Store((lane + 64) * 4, 0u);
        return;
    }
    for (uint i = lane; i < STATS_SET; i += 64) gs_keys[i] = gs_entries[i] = 0;
    if (lane == 0) gs_keyCount = gs_entryCount = 0;
    giCacheTileClear(lane);

    ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[0].w];
    const GiHeader h = giHeader(cache);
    bool valid = all(pixel < P[1].xy);
    const float d = valid ? depth.Load(int3(pixel, 0)) : 0;
    valid = valid && d > 0;
    float3 position = 0, n = float3(0, 0, 1);
    if (valid)
    {
        const GBufferSample g = decodeGBuffer(gbuffer.Load(int3(pixel, 0)));
        position = worldFromDepth(float2(pixel), d);
        const float3 v = normalize(g_cameraPosition - position);
        n = dot(g.normal, v) > 0 ? g.normal : -g.normal;
    }
    GroupMemoryBarrierWithGroupSync();
    giCacheTileFile(h, valid, position, n);
    GroupMemoryBarrierWithGroupSync();
    giCacheTileResolve(cache, h, lane);
    GroupMemoryBarrierWithGroupSync();
    uint levels = 0, lookups = 0, slots = 0, found = 0, missed = 0, evaluated = 0;
    if (valid)
    {
        {
            // The group-resolved lookup against the per-pixel one (bit for bit).
            float wa, wb;
            const float3 a = giCacheIrradianceScreen(cache, h, position, n, wa);
            const float3 b = giCacheIrradianceTile(cache, h, position, n, wb);
            if (any(asuint(a) != asuint(b)) || asuint(wa) != asuint(wb))
            {
                counters.InterlockedAdd(44, 1u);
                const float rel = max(max(abs(a.x - b.x), abs(a.y - b.y)), abs(a.z - b.z)) / max(max(max(a.x, a.y), a.z), 1e-20);
                counters.InterlockedMax(48, asuint(rel));
            }
        }
        uint level;
        const float beta = giLevelBand(h, position, level);
        const uint nc = giNormalClass(n);
        float remain = 1;
        [loop] for (uint k = 0; k < GI_FILL_LEVELS && remain > 1e-3 && level <= h.maxLevel; ++k, ++level)
        {
            ++levels;
            const float s = giCellSize(h, level);
            const float3 f = position / s - 0.5;
            const int3 c0 = int3(floor(f));
            const float3 t = f - floor(f);
            uint entries[8];
            [loop] for (uint c = 0; c < 8; ++c)
            {
                const int3 o = int3(c & 1, (c >> 1) & 1, c >> 2);
                const uint64_t key = giKey(level, nc, c0 + o);
                ++lookups;
                {
                    const uint tag = giHash(key) | 1u;
                    uint ts = tag & (GI_TILE_SLOTS - 1);
                    bool held = false;
                    [loop] for (uint q = 0; q < 16 && !held; ++q)
                    {
                        const uint tt = gs_giTileTag[ts];
                        if (tt == GI_TILE_EMPTY) break;
                        held = tt == tag && all(gs_giTileKey[ts] == uint2((uint)key, (uint)(key >> 32)));
                        ts = (ts + 1) & (GI_TILE_SLOTS - 1);
                    }
                    if (!held) ++missed;
                }
                statsInsert(0, giHash(key) | 1u);
                uint entry = statsFind(cache, h, key, slots);
                if (entry != GI_ENTRY_PENDING && cache.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) == 0) entry = GI_ENTRY_PENDING;
                entries[c] = entry;
                if (entry == GI_ENTRY_PENDING) continue;
                ++found;
                statsInsert(1, entry + 1);
            }
            uint has = 0;
            [unroll] for (uint c = 0; c < 8; ++c)
                if (entries[c] != GI_ENTRY_PENDING) has |= 1u << c;
            float w = 0;
            [unroll] for (uint c = 0; c < 8; ++c)
            {
                const float wc = giScreenCornerWeight(has, c, t, n * n);
                if (wc <= 0) continue;
                w += wc;
                ++evaluated;
            }
            const float take = k == 0 ? 1 - beta : 1.0;
            remain *= 1 - take * w;
        }
        counters.InterlockedAdd(0, 1u);
        counters.InterlockedAdd(4, levels);
        counters.InterlockedAdd(8, lookups);
        counters.InterlockedAdd(12, slots);
        counters.InterlockedAdd(16, found);
        if (levels > 1) counters.InterlockedAdd(40, 1u);
        counters.InterlockedAdd(52, missed);
        counters.InterlockedAdd(56, evaluated);
        {
            // Against the reconstruction before the partner corners (the missing weight filled from coarser levels).
            float wo, wn;
            const float3 o = statsFillOnly(cache, h, position, n, wo);
            const float3 e = giCacheIrradianceScreen(cache, h, position, n, wn);
            if (wo > 0 && wn > 0)
            {
                const float rel = max(max(abs(e.x - o.x), abs(e.y - o.y)), abs(e.z - o.z)) / max(max(max(o.x, o.y), o.z), 1e-20);
                counters.InterlockedMax(320, asuint(rel));
                counters.InterlockedAdd(324, (uint)min(rel * 1e3, 1e5));
                counters.InterlockedAdd(328, 1u);
                if (rel > 0.01) counters.InterlockedAdd(332, 1u);
                if (rel > 0.05) counters.InterlockedAdd(336, 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && gs_keyCount > 0)
    {
        counters.InterlockedAdd(20, 1u);
        counters.InterlockedAdd(24, gs_keyCount);
        counters.InterlockedAdd(28, gs_entryCount);
        counters.InterlockedMax(32, gs_keyCount);
        counters.InterlockedMax(36, gs_entryCount);
        counters.InterlockedAdd(64 + 4 * min(gs_entryCount, 63u), 1u);
    }
}
