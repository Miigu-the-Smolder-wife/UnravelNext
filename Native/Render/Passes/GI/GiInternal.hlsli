// R-internal GI helpers: cache writes (entry creation, requests), SH blocks, probe records. Not for other tracks.
#ifndef UNX_GI_INTERNAL_HLSLI
#define UNX_GI_INTERNAL_HLSLI
#include "Passes/GI/GiCache.hlsli"

// Header byte offsets (GiCache.hlsli GiHeader) of the fields kernels write.
#define GI_H_FREE_COUNT 48
#define GI_H_UPDATE_COUNT 52
#define GI_H_SELECTED_COUNT 56
#define GI_H_FRAME 60
#define GI_H_BG_CURSOR 64
#define GI_H_BG_COUNT 68
#define GI_H_EPOCH 72
#define GI_H_LIVE_COUNT 76
#define GI_H_CAMERA 80
#define GI_H_HIT_COUNT 112        // + 4 * parity
// Statistics (diagnostics only) and the selection state of this frame.
#define GI_H_STAT_CREATED 128
#define GI_H_STAT_ALLOC_FAIL 132
#define GI_H_STAT_TABLE_FULL 136
#define GI_H_STAT_EVICTED 140
#define GI_H_STAT_RESETS 144
// Byte offset of the per-entry radiance map owner (lowest probe index reading the entry this frame; ~0 = none), written by
// GiProbeGather, read by GiProbeMaps, reset by GiTableClear.
#define GI_H_MAP_OWNER 148
// Selection of this frame, per tier (0 = screen, 1 = hit): age bucket at the tier's budget boundary, entries still to
// take from that bucket, atomic counter within it (tier t at GI_H_SELECT + 16 t).
#define GI_H_SELECT 160
// Age histograms of the requested entries, one per tier (64 buckets each: frames since the last update, 63 = 63 or more
// or never updated). Tier 0 = entries the screen probes read, tier 1 = entries last frame's GI rays read (bounces).
#define GI_HISTOGRAM 256
#define GI_AGE_BUCKETS 64u
#define GI_TIER_HIT 0x80000000u  // flag on update-list entries requested by hits

// Meta (16 B): keyLo, keyHi (0 = free), last used frame, frame of the last update-list insertion.
void giTouch(RWByteAddressBuffer b, GiHeader h, uint entry) { b.Store(h.offMeta + entry * 16 + 8, h.frame); }

// Appends the entry to this frame's request list once, in tier 0 (screen) or 1 (hit). Screen requests are made first, so
// an entry both on screen and hit stays in tier 0.
void giRequestUpdate(RWByteAddressBuffer b, GiHeader h, uint entry, uint tier)
{
    uint previous;
    b.InterlockedExchange(h.offMeta + entry * 16 + 12, h.frame, previous);
    if (previous == h.frame) return;
    uint slot;
    b.InterlockedAdd(GI_H_UPDATE_COUNT, 1u, slot);
    if (slot < h.capacity) b.Store(h.offUpdate + slot * 4, entry | (tier != 0 ? GI_TIER_HIT : 0u));
}

uint giAgeBucket(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    const uint last = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE);
    return last == 0 ? GI_AGE_BUCKETS - 1 : min(h.frame - last, GI_AGE_BUCKETS - 1);
}

// Records an entry that a GI ray hit this frame (its irradiance fed a bounce): requested for update next frame.
void giRequestHit(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    uint previous;
    b.InterlockedExchange(h.offHitStamp + entry * 4, h.frame, previous);
    if (previous == h.frame) return;
    const uint parity = h.frame & 1u;
    uint slot;
    b.InterlockedAdd(GI_H_HIT_COUNT + 4 * parity, 1u, slot);
    if (slot < h.capacity) b.Store(h.offHitList + (parity * h.capacity + slot) * 4, entry);
}

// Cache lookups by ray hits (GiCache.hlsli giKeepRead): each contributing entry is touched and requested once per frame
// (a plain load of its hit stamp first, so entries shared by many rays cost one load after the first).
void giKeepRead(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    if (b.Load(h.offHitStamp + entry * 4) == h.frame) return;
    giTouch(b, h, entry);
    giRequestHit(b, h, entry);
}

uint giPackNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16);
}

// Anchor of an entry created at a ray hit: the hit point stepped back along the ray that found it (2 mm + 0.04 % of the
// distance to the camera, twice the GI ray origin offset). The hit lies on the boundary of free space, possibly on a crease
// where another face meets it; the ray came through free space, so the step moves the anchor off every face it touches,
// and the entry's own rays (anchor + normal offset) no longer start on a neighbouring face's plane.
float3 giAnchorAtHit(GiHeader h, float3 hitPosition, float3 rayDirection)
{
    return hitPosition - rayDirection * (2e-3 + 4e-4 * distance(hitPosition, h.camera));
}

// Finds the entry of 'key' or creates it with the given anchor. Returns GI_ENTRY_PENDING when the entry is being
// created by another thread in this pass, the pool is exhausted or the probe sequence is full (counted in the header).
uint giFindOrCreate(RWByteAddressBuffer b, GiHeader h, uint64_t key, float3 anchor, float3 normal, out bool created)
{
    created = false;
    uint slot = giHash(key) & (h.tableSlots - 1);
    [loop] for (uint i = 0; i < GI_PROBE_LIMIT; ++i)
    {
        const uint address = h.offTable + slot * 16;
        uint64_t previous;
        b.InterlockedCompareExchange64(address, 0ull, key, previous);
        if (previous == key) return b.Load(address + 8);
        if (previous == 0)
        {
            uint count;
            b.InterlockedAdd(GI_H_FREE_COUNT, 0xFFFFFFFFu, count);
            if ((int)count <= 0)
            {
                b.InterlockedAdd(GI_H_FREE_COUNT, 1u);
                b.InterlockedAdd(GI_H_STAT_ALLOC_FAIL, 1u);
                return GI_ENTRY_PENDING;  // the slot stays pending until the next rehash
            }
            const uint entry = b.Load(h.offFree + (count - 1) * 4);
            b.Store4(h.offMeta + entry * 16, uint4((uint)key, (uint)(key >> 32), h.frame, 0));
            b.Store4(h.offAnchor + entry * 16, uint4(asuint(anchor), giPackNormal(normal)));
            [unroll] for (uint k = 0; k < GI_SH_STRIDE / 16; ++k) b.Store4(h.offSh + entry * GI_SH_STRIDE + k * 16, 0u);
            [loop] for (uint t = 0; t < GI_TEXEL_COUNT * 8 / 16; ++t) b.Store4(h.offTexels + entry * GI_TEXEL_COUNT * 8 + t * 16, 0u);
            b.Store(h.offHitStamp + entry * 4, 0u);
            b.Store(address + 8, entry);
            b.InterlockedAdd(GI_H_STAT_CREATED, 1u);
            created = true;
            return entry;
        }
        slot = (slot + 1) & (h.tableSlots - 1);
    }
    b.InterlockedAdd(GI_H_STAT_TABLE_FULL, 1u);
    return GI_ENTRY_PENDING;
}

// The entry at a surface point: its level (at least minLevel), cell and normal class.
uint64_t giSurfaceKey(GiHeader h, float3 p, float3 n, uint minLevel)
{
    const uint level = max(giLevel(h, p), minLevel);
    return giKey(level, giNormalClass(n), int3(floor(p / giCellSize(h, level))));
}

// History weight of an update: pure Jacobi (alpha 1) for the first jacobiUpdates updates after a reset (multi-bounce
// converges geometrically), then the running mean over at most historyMax updates (static scenes converge; slow changes
// such as a moving sun are followed with that window).
float giHistoryAlpha(GiHeader h, uint history)
{
    if (history < h.jacobiUpdates) return 1.0;
    return max(1.0 / (float)(history - h.jacobiUpdates + 2), 1.0 / (float)h.historyMax);
}

// Updates since the last reset, 0 when the entry's history belongs to an older lighting epoch.
template <typename B>
uint giHistory(B b, GiHeader h, uint entry)
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    return b.Load(a + GI_SH_EPOCH) == h.epoch ? b.Load(a + GI_SH_HISTORY) : 0u;
}

// SH block coefficients (irradiance, world frame).
template <typename B>
void giLoadSh(B b, GiHeader h, uint entry, out float3 c[9])
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32), w3 = b.Load4(a + 48);
    const uint w[14] = { w0.x, w0.y, w0.z, w0.w, w1.x, w1.y, w1.z, w1.w, w2.x, w2.y, w2.z, w2.w, w3.x, w3.y };
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const uint i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        c[k] = float3(f16tof32(w[i0 >> 1] >> ((i0 & 1) * 16)), f16tof32(w[i1 >> 1] >> ((i1 & 1) * 16)), f16tof32(w[i2 >> 1] >> ((i2 & 1) * 16))) * GI_LOAD_SCALE;
    }
}

// Trilinear SH coefficients at a surface point (the cells of its level and normal class that exist; next coarser level
// when none does). Returns the total weight (0 = nothing cached).
template <typename B>
float giCacheShAt(B b, GiHeader h, float3 p, float3 normal, out float3 c[9], out uint bestEntry)
{
    [unroll] for (uint k = 0; k < 9; ++k) c[k] = 0;
    bestEntry = GI_ENTRY_PENDING;
    float bestWeight = 0;
    const uint nc = giNormalClass(normal);
    uint level = giLevel(h, p);
    float weight = 0;
    [loop] for (uint attempt = 0; attempt < 2 && weight <= 0; ++attempt, ++level)
    {
        const float s = giCellSize(h, level);
        const float3 f = p / s - 0.5;
        const int3 c0 = int3(floor(f));
        const float3 t = f - floor(f);
        [loop] for (uint k = 0; k < 8; ++k)
        {
            const int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
            const uint entry = giFind(b, h, giKey(level, nc, c0 + o));
            if (entry == GI_ENTRY_PENDING || b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) == 0) continue;  // no information yet
            const float3 wt = lerp(1 - t, t, float3(o));
            const float w = wt.x * wt.y * wt.z;
            float3 e[9];
            giLoadSh(b, h, entry, e);
            [unroll] for (uint j = 0; j < 9; ++j) c[j] += w * e[j];
            weight += w;
            if (w > bestWeight)
            {
                bestWeight = w;
                bestEntry = entry;
            }
        }
    }
    if (weight > 0)
        [unroll] for (uint j = 0; j < 9; ++j) c[j] /= weight;
    return weight;
}

// The entry of update slot 'slot' this frame: selected entries first, then the background range. False = no entry.
bool giUpdateSlot(RWByteAddressBuffer b, GiHeader h, uint slot, out uint entry, out bool background)
{
    background = slot >= h.selectedCount;
    if (!background)
    {
        entry = b.Load(h.offSelected + slot * 4);
        return true;
    }
    const uint i = slot - h.selectedCount;
    entry = (h.backgroundCursor + i) % h.capacity;
    if (i >= h.backgroundCount) return false;
    if (b.Load(h.offMeta + entry * 16 + 4) == 0) return false;                               // free
    if (b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE) == h.frame) return false;  // selected this frame
    return true;
}

// ---- screen probes

// The probe pixel of a tile: its centre, else the first of four inner points that has geometry (stable while the
// camera is still). Returns false when the tile shows only sky at those points.
bool giProbePixel(Texture2D<float> depth, uint2 tile, uint spacing, uint2 size, out uint2 offset, out float deviceDepth)
{
    const uint q = spacing / 4, c = spacing / 2;
    const uint2 candidates[5] = { uint2(c, c), uint2(q, q), uint2(3 * q, q), uint2(q, 3 * q), uint2(3 * q, 3 * q) };
    [unroll] for (uint i = 0; i < 5; ++i)
    {
        const uint2 pixel = min(tile * spacing + candidates[i], size - 1);
        const float d = depth.Load(int3(pixel, 0));
        if (d > 0)
        {
            offset = pixel - tile * spacing;
            deviceDepth = d;
            return true;
        }
    }
    offset = uint2(c, c);
    deviceDepth = 0;
    return false;
}

uint giPackHalf2(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }

// Shared-exponent RGB (5-bit exponent, bias 15, 9-bit mantissas), non-negative.
uint giPackRgb9e5(float3 c)
{
    c = clamp(c, 0.0, 65408.0);
    const float m = max(c.r, max(c.g, c.b));
    int e = max(-16, (int)floor(log2(max(m, 1e-30)))) + 16;  // biased exponent of the largest component + 1
    float scale = exp2((float)e - 24.0);
    uint3 q = (uint3)round(c / scale);
    if (max(q.r, max(q.g, q.b)) >= 512u)
    {
        ++e;
        scale *= 2;
        q = (uint3)round(c / scale);
    }
    return min(q.r, 511u) | (min(q.g, 511u) << 9) | (min(q.b, 511u) << 18) | ((uint)clamp(e, 0, 31) << 27);
}

// Probe record (ScreenProbes.hlsli decodes it): block (8i .. 8i+7, 4j .. 4j+3), record in row 0 texels 0-3.
// Probe record (ScreenProbes.hlsli): plane 0 = { world position (fp32 x 3), octahedral normal (0 = no surface) }, the
// footprint's only read; planes 1-4 = 27 fp16 SH coefficients x GI_STORE_SCALE and unorm16 occlusion.
void giStoreProbe(RWTexture2D<uint4> t, uint2 probe, uint2 count, float3 c[9], float3 position, float3 normal, float occlusion, bool valid)
{
    float v[27];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        v[3 * k] = c[k].r * GI_STORE_SCALE;
        v[3 * k + 1] = c[k].g * GI_STORE_SCALE;
        v[3 * k + 2] = c[k].b * GI_STORE_SCALE;
    }
    uint w[14];
    [unroll] for (uint i = 0; i < 13; ++i) w[i] = giPackHalf2(v[2 * i], v[2 * i + 1]);
    w[13] = (f32tof16(v[26]) & 0xFFFFu) | (uint(round(saturate(occlusion) * 65535.0)) << 16);
    const uint normalWord = valid ? max(giPackNormal(normal), 1u) : 0u;
    const uint y = count.y * 4 + probe.y;
    t[uint2(probe.x, y)] = uint4(asuint(position.x), asuint(position.y), asuint(position.z), normalWord);
    t[uint2(probe.x + count.x, y)] = uint4(w[0], w[1], w[2], w[3]);
    t[uint2(probe.x + 2 * count.x, y)] = uint4(w[4], w[5], w[6], w[7]);
    t[uint2(probe.x + 3 * count.x, y)] = uint4(w[8], w[9], w[10], w[11]);
    t[uint2(probe.x + 4 * count.x, y)] = uint4(w[12], w[13], 0, 0);
}

// Solid angle weight of texel (x, y) of an n x n hemispherical octahedral map (midpoint of 2 dA_uv / |v|^3).
float giMapTexelWeight(uint2 texel, uint n)
{
    const float2 uv = (float2(texel) + 0.5) / n;
    const float2 q = uv * 2 - 1;
    const float2 p = float2(q.x + q.y, q.x - q.y) * 0.5;
    const float l = length(float3(p, 1 - abs(p.x) - abs(p.y)));
    return 1.0 / (l * l * l);
}

#endif
