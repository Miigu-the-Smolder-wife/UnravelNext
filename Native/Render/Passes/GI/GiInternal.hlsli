// R-internal GI helpers: cache writes (entry creation, requests), SH blocks, probe records. Not for other tracks.
#ifndef UNX_GI_INTERNAL_HLSLI
#define UNX_GI_INTERNAL_HLSLI
#include "Passes/GI/GiCache.hlsli"
#include "RayTracing/HalfNearest.hlsli"

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
// Reflection hits' cache lookups this frame, and those that found no updated cell at any level (diagnostics).
#define GI_H_STAT_HIT_LOOKUPS 152
#define GI_H_STAT_HIT_MISSES 156
// Reflection G samples this frame, and those whose estimate took the ratio branch (beta < 1 in some channel; diagnostics).
#define GI_H_STAT_G_SAMPLES 192
#define GI_H_STAT_G_RATIO 196
// Histogram of the G samples' luminance ratio mean(L) / mean(g) (the rays over the control variate): 9 bins of log2 in
// [-4, 5), the first and last also take what lies beyond (diagnostics).
#define GI_H_STAT_G_HIST 200
#define GI_G_HIST_BINS 9u
// G samples whose control variate is zero (mean g = 0: the cache predicts no light along every ray; not in the histogram).
#define GI_H_STAT_G_ZERO 236
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

// Deterministic update selection (gi.deterministic; GiDetDigits, GiDetResolve, GiSelect): per tier a 256-bin digit
// histogram, then the priority prefix resolved so far, the entries still to take and whether the entries whose priority
// equals the final prefix are taken.
#define GI_DET_PREFIX 1024
#define GI_DET_REMAINING 1028
#define GI_DET_EQUAL 1032
#define GI_DET_TIER_BYTES 1040

// An entry's selection priority this frame: a hash of its key (not its index, which depends on allocation order) and
// the frame.
uint giDetHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint giDetPriority(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    const uint2 key = b.Load2(h.offMeta + entry * 16);
    return giDetHash(key.x ^ giDetHash(key.y ^ (h.frame * 0x9E3779B9u)));
}
// A background candidate (deterministic mode): live and not updated this frame.
bool giDetBackgroundCandidate(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    return b.Load(h.offMeta + entry * 16 + 4) != 0 && b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE) != h.frame;
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
static bool g_giKeepReads = true;  // per thread; false: lookups read only (attribution runs, reflection experiment 256)
// Per thread: the trilinear weight of young entries (GiInternal giYoung: still in their Jacobi phase) among the RW lookups'
// reads since the caller zeroed it (GiTrace's bounce fallback: whether the irradiance it read is a converged estimate).
static float g_giReadYoung = 0;
static bool g_giTrackYoung = false;  // per thread: accumulate g_giReadYoung (GiTrace's bounce fallback sets it around its read;
                                     // the other RW readers - reflection hits - do not use it and skip its two loads per entry)
// The entry's Jacobi length (GiIntegrate): updates until the multi-bounce iteration's residual s^J is below
// GI_JACOBI_RESIDUAL, s = the share of the entry's irradiance its rays read from other cells (bounce light). The fixed
// gi.jacobi_updates (8, rho^8 < 0.4 % at albedo 0.5) is the minimum: a white-tiled room (s ~ 0.9) needs 53. After the
// Jacobi phase the running mean mixes old iterates in, which converges at 1 - alpha (1 - s) per update (0.997 at s 0.9,
// alpha 1/32): the rho = 0.9 furnace was still -16 % after 600 frames, the bathhouse's indirect light -36 % [measured,
// 2026-09-27].
#define GI_JACOBI_RESIDUAL 0.004
// Stored in the history word's top byte in units of 16 updates (0 after a reset or an epoch change: gi.jacobi_updates).
template <typename B>
uint giJacobiLength(B b, GiHeader h, uint entry)
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    const uint history = b.Load(a + GI_SH_EPOCH) == h.epoch ? b.Load(a + GI_SH_HISTORY) : 0u;
    return max((history >> 24) * 16u, h.jacobiUpdates);
}
void giKeepRead(RWByteAddressBuffer b, GiHeader h, uint entry, float w)
{
    if (g_giTrackYoung)
    {
        const uint a = h.offSh + entry * GI_SH_STRIDE;
        const uint history = b.Load(a + GI_SH_EPOCH) == h.epoch ? b.Load(a + GI_SH_HISTORY) : 0u;
        if ((history & 0xFFFu) < max((history >> 24) * 16u, h.jacobiUpdates)) g_giReadYoung += w;  // giYoung
    }
    if (!g_giKeepReads) return;
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
// Deterministic anchors (gi.deterministic): an entry's anchor is not its creator's point (which thread creates a cell
// depends on arrival order) but the minimum of every candidate's packed point and normal while the entry has never been
// updated; GiDetAnchors decodes it before the frame's rays leave anchors. 64 bits: position in [cell - s/2, cell + 3s/2]
// per axis at 14 bits (2s / 16383), octahedral normal at 2 x 11 bits.
uint64_t giPackAnchorCandidate(GiHeader h, uint64_t key, float3 p, float3 n)
{
    const uint level = (uint)(key & 31u);
    const float s = giCellSize(h, level);
    const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;  // sign-extend 18 bits
    const float3 origin = float3(cell) * s - 0.5 * s;
    const uint3 q = (uint3)clamp(round((p - origin) / (2 * s) * 16383.0), 0.0, 16383.0);
    float2 e = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0) e = (1.0 - abs(e.yx)) * select(e >= 0.0, 1.0, -1.0);
    const uint2 u = (uint2)clamp(round((e * 0.5 + 0.5) * 2047.0), 0.0, 2047.0);
    return ((uint64_t)q.x << 50) | ((uint64_t)q.y << 36) | ((uint64_t)q.z << 22) | ((uint64_t)u.x << 11) | (uint64_t)u.y;
}
void giDetAnchorCandidate(RWByteAddressBuffer b, GiHeader h, uint entry, uint64_t key, float3 p, float3 n)
{
    if ((h.flags & 1u) == 0 || entry == GI_ENTRY_PENDING) return;
    if (b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) != 0) return;  // anchored for good once updated
    uint64_t previous;
    b.InterlockedMin64(h.offAnchorMin + entry * 8, giPackAnchorCandidate(h, key, p, n), previous);
}

uint giFindOrCreate(RWByteAddressBuffer b, GiHeader h, uint64_t key, float3 anchor, float3 normal, out bool created)
{
    created = false;
    // Existing entries (nearly every call) by plain loads; the compare-exchange probe below only when the key is absent.
    const uint existing = giFind(b, h, key);
    if (existing != GI_ENTRY_PENDING)
    {
        giDetAnchorCandidate(b, h, existing, key, anchor, normal);
        return existing;
    }
    uint slot = giHash(key) & (h.tableSlots - 1);
    [loop] for (uint i = 0; i < GI_PROBE_LIMIT; ++i)
    {
        const uint address = h.offTable + slot * 16;
        uint64_t previous;
        b.InterlockedCompareExchange64(address, 0ull, key, previous);
        if (previous == key)
        {
            const uint found = b.Load(address + 8);
            if ((h.flags & 1u) != 0 && found == GI_ENTRY_PENDING)
            {
                // Its creator has not published the entry yet: the candidate goes to the table slot (GiDetFold), so the
                // anchor does not depend on arrival order.
                uint64_t previousSlot;
                b.InterlockedMin64(h.offSlotAnchor + slot * 8, giPackAnchorCandidate(h, key, anchor, normal), previousSlot);
            }
            giDetAnchorCandidate(b, h, found, key, anchor, normal);
            return found;
        }
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
            [loop] for (uint t = 0; t < GI_IRR_STRIDE / 16; ++t) b.Store4(h.offIrr + entry * GI_IRR_STRIDE + t * 16, 0u);
            [loop] for (uint t = 0; t < GI_TEXEL_COUNT * 4 / 16; ++t) b.Store4(giEmitterOffset(h) + entry * GI_TEXEL_COUNT * 4 + t * 16, 0u);
            b.Store(h.offHitStamp + entry * 4, 0u);
            if (h.flags & 1u)
            {
                b.Store2(h.offAnchorMin + entry * 8, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
                uint64_t previous;
                b.InterlockedMin64(h.offAnchorMin + entry * 8, giPackAnchorCandidate(h, key, anchor, normal), previous);
                DeviceMemoryBarrier();  // the candidate before the entry is published
            }
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

// History word (GI_SH_HISTORY, 0 after a reset): bits 0-11 = convergence phase (saturating), bits 12-23 = updates in the
// running mean, bits 24-31 = the Jacobi length / 16 (rounded up; giJacobiLength).
//   An update's only biased input is the bounce light it reads from other cells (sky, sun shadow rays, local and emissive
//   samples are unbiased from the first update). A cell is young while its phase is < its Jacobi length (giJacobiLength): its value still
//   carries the Jacobi iteration's error (multi-bounce converges geometrically from the reset). The weight of an update
//   is Jacobi (alpha 1: replace) by the share y of its bounce reads (texel rays whose hit has a bounce term) that came from
//   young cells or found no data, and the running mean over at most historyMax updates for the rest (counted, not
//   luminance-weighted: a read without data returns 0, the most biased read has no luminance):
//     alpha = lerp(1 / (n + 1), 1, y) while the cell itself is young, 1 / (n + 1) after (>= 1 / historyMax),
//     n' = max(1, round(lerp(n + 1, 1, y))).
//   After a reset of the whole cache every read is young: the same Jacobi phase, then a mean restarted from its last
//   iterate, as before (the furnace converges as fast). A cell made later in a converged cache (camera motion, new
//   geometry) reads converged cells: y = 0, a plain mean from its first update and never young (its phase jumps to
//   jacobiUpdates), so it shows no single-update noise (the 8 replaced updates showed each cell's 64-ray estimate
//   alone: cell-sized blotches, D0 play capture 2026-09-26). Past its own young phase a cell averages whatever it reads
//   (as before): bounce cells are updated less often than screen cells, and waiting for them kept readers replacing.
uint giHistoryPhase(uint history) { return history & 0xFFFu; }
uint giHistorySamples(uint history) { return (history >> 12) & 0xFFFu; }
uint giHistoryJacobi(uint history) { return (history >> 24) * 16u; }
// cap: the running mean's window for this update (GiIntegrate: historyMax after a change, historyStatic while steady).
float giHistoryAlpha(GiHeader h, uint history, float young, uint jacobi, uint cap)
{
    const float mean = max(1.0 / (float)(giHistorySamples(history) + 1), 1.0 / (float)cap);
    return giHistoryPhase(history) < jacobi ? lerp(mean, 1.0, saturate(young)) : mean;
}
uint giHistoryNext(GiHeader h, uint history, float young, uint jacobi, uint cap)
{
    if (giHistoryPhase(history) >= jacobi) young = 0;
    const float n = lerp((float)(giHistorySamples(history) + 1), 1.0, saturate(young));
    const uint samples = clamp((uint)round(n), 1u, max(cap, 2u) - 1u);
    const uint phase = young > 0 ? giHistoryPhase(history) + 1 : max(giHistoryPhase(history) + 1, jacobi);
    return min(phase, 0xFFFu) | (min(samples, 0xFFFu) << 12) | (min((jacobi + 15) / 16, 255u) << 24);
}
// History word of the entry, 0 when it belongs to an older lighting epoch.
template <typename B>
uint giHistory(B b, GiHeader h, uint entry)
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    return b.Load(a + GI_SH_EPOCH) == h.epoch ? b.Load(a + GI_SH_HISTORY) : 0u;
}
template <typename B>
bool giYoung(B b, GiHeader h, uint entry) { return giHistoryPhase(giHistory(b, h, entry)) < giJacobiLength(b, h, entry); }

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
// Screen probe (i, j) sits at the pixel corner (spacing i, spacing j) (design revision 12.3: the four probes around a
// tile are its corners, shared with its neighbours). Its surface point: the first candidate pixel around the corner
// that is not sky (the pixel just below-right of the corner, then the four at +-spacing/4), clamped into the view.
bool giProbePixel(Texture2D<float> depth, uint2 probe, uint spacing, uint2 size, out uint2 pixel, out float deviceDepth)
{
    const int q = (int)spacing / 4;
    const int2 candidates[5] = { int2(0, 0), int2(-q, -q), int2(q, -q), int2(-q, q), int2(q, q) };
    [unroll] for (uint i = 0; i < 5; ++i)
    {
        const uint2 p = (uint2)clamp(int2(probe * spacing) + candidates[i], int2(0, 0), int2(size) - 1);
        const float d = depth.Load(int3(p, 0));
        if (d > 0)
        {
            pixel = p;
            deviceDepth = d;
            return true;
        }
    }
    pixel = min(probe * spacing, size - 1);
    deviceDepth = 0;
    return false;
}

// fp16 pairs rounded to nearest even (RayTracing/HalfNearest.hlsli): the hardware conversion truncates toward zero, and
// the texels and SH are running means whose steady state would carry that bias / alpha (-0.8 % at alpha 1/32).
uint giPackHalf2(float a, float b) { return f32tof16(nearestHalf(a)) | (f32tof16(nearestHalf(b)) << 16); }

// Stochastic rounding for running means (GiIntegrate): a mean over n updates moves by (x - m) / n per update, below
// half a step of fp16 or RGB9E5 for most samples once n is large, and round-to-nearest then holds the median of a skewed
// sample distribution instead of its mean (-5 % at n = 256 with 10 % bright samples [offline, 2026-09-27]). One step of
// uniform dither before rounding to nearest makes the stored value's expectation exact. u in [0, 1).
float giDitherHalf(float x, float u)
{
    const float ax = abs(x);
    const float step = ax < 6.103515625e-5 ? 5.9604644775390625e-8 : exp2(floor(log2(ax)) - 10.0);  // fp16 spacing at x
    return x + (u - 0.5) * step;
}
uint giDitherHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float giDitherUnit(uint x) { return (giDitherHash(x) >> 8) * (1.0 / 16777216.0); }

// Shared-exponent RGB (5-bit exponent, bias 15, 9-bit mantissas), non-negative. dither: per-channel offsets in [0, 1)
// added before truncation (stochastic rounding), or 0.5 for round to nearest.
uint giPackRgb9e5(float3 c, float3 dither = float3(0.5, 0.5, 0.5))
{
    c = clamp(c, 0.0, 65408.0);
    const float m = max(c.r, max(c.g, c.b));
    int e = max(-16, (int)floor(log2(max(m, 1e-30)))) + 16;  // biased exponent of the largest component + 1
    float scale = exp2((float)e - 24.0);
    uint3 q = (uint3)floor(c / scale + dither);
    if (max(q.r, max(q.g, q.b)) >= 512u)
    {
        ++e;
        scale *= 2;
        q = (uint3)floor(c / scale + dither);
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
    w[13] = (f32tof16(nearestHalf(v[26])) & 0xFFFFu) | (uint(round(saturate(occlusion) * 65535.0)) << 16);
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
