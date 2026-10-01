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
// The entry's identity for its ray seeds, texel rotations and dither in deterministic mode: its key, hashed, without the
// frame (giDetPriority's frame term re-drew the texels' R2 rotation every frame: the low-discrepancy walk over the
// entry's updates became independent jitter - a noisier cache in deterministic mode than in the default one).
template <typename B>
uint giDetKey(B b, GiHeader h, uint entry)
{
    const uint2 key = b.Load2(h.offMeta + entry * 16);
    return giDetHash(key.x ^ giDetHash(key.y));
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

// ---- Redesign V2 P1 (RENDERER_REDESIGN_V2 1.1): update tiers, the parent prior, relight restarts.
// Header words (the spare bytes after the admission word, GiSystem.cpp h[195..]):
#define GI_P1_FLAGS 780         // bit 0 gi.update_tiers, bit 1 gi.parent_prior, bit 2 gi.relight_restart, bit 3 gi.hit_light_footprint
#define GI_P1_T0_SHARE 784      // gi.young_update_share (float): the most of a tier's updates the young (T0) entries take
#define GI_P1_DELTA2 788        // running estimate of the parent-child relative squared difference (float), kept by GiBegin
#define GI_P1_DELTA_SUM 792     // this frame's samples of it: sum of min(d^2, 1) x 2^16, count (GiIntegrate)
#define GI_P1_DELTA_COUNT 796
#define GI_P1_STAT_PRIORS 800   // statistics of this frame: updates that started from a parent prior, relight restarts,
#define GI_P1_STAT_RESTARTS 804 // T0 and T1 entries selected
#define GI_P1_STAT_T0 808
#define GI_P1_STAT_T1 812
#define GI_P1_FOOTPRINT_SCALE 848  // gi.hit_light_footprint_scale (float, diagnostics: the footprint's side over the texel cone's)
// Energy audit (gi.experiment_disable 32768, diagnostics): over every GI hit whose chosen local light is a point or spot
// light, the luminance of f x weight with the point diffuse term and with its footprint mean, without and with the hit's
// visibility, and the same four for hits whose point term exceeds 1000 nit; x 1024, 64-bit sums since the cache's creation.
#define GI_AUDIT_SUMS 856       // 8 x uint64: pt, fp, pt V, fp V, then the > 1000 nit subset
// Bounce split (gi.bounce_split, GI_P1_FLAGS bit 4; RENDERER_REDESIGN_V2 11.2): per entry 48 B at the word's offset, the
// L1 radiance SH (world frame, fp16 x GI_STORE_SCALE, 4 coefficients x RGB) of the bounce part B of the entry's rays, twice:
// the current B (blended with max(1 / (n + 1), 1 / window), window 1 = Jacobi replacement) and its long mean Bm (the
// entry's own weights). GiIntegrate keeps the map, SH and texels as the long mean M of the whole samples and stores
// M + (B - Bm) projected: the non-bounce part keeps its long mean, the bounce part follows the neighbours' current values.
#define GI_BSPLIT_OFFSET 920
#define GI_BSPLIT_WINDOW 924    // gi.bounce_split_updates (the current B's window)
void giBounceL1Load(RWByteAddressBuffer b, uint entry, out float3 cur[4], out float3 mean[4])
{
    const uint a = b.Load(GI_BSPLIT_OFFSET) + entry * 48;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32);
    const uint w[12] = { w0.x, w0.y, w0.z, w0.w, w1.x, w1.y, w1.z, w1.w, w2.x, w2.y, w2.z, w2.w };
    float v[24];
    [unroll] for (uint i = 0; i < 24; ++i) v[i] = f16tof32(w[i >> 1] >> ((i & 1) * 16)) * GI_LOAD_SCALE;
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        cur[c] = float3(v[3 * c], v[3 * c + 1], v[3 * c + 2]);
        mean[c] = float3(v[12 + 3 * c], v[13 + 3 * c], v[14 + 3 * c]);
    }
}
#define GI_H_SELECT_T0 816      // per request tier (16 B each): the young range's threshold bucket, quota, fill counter
// Priority buckets with the tiers on (gi.update_tiers): 60..63 = T0 (fewer than 4 measured updates since the entry's creation
// or restart; 63 = none), 48..59 = T1 (4..15; fewer first), 0..47 = the rest by age (frames since the last update, 47 = 47
// or more, or never updated). Selection takes buckets from the top: the young cells converge first (a disocclusion, a cut,
// a relit area), the rest keeps the stalest-first rotation. Off: the age bucket alone (the previous rule).
#define GI_T0_BUCKET 60u
#define GI_T1_BUCKET 48u
// The entry's measured updates since its creation or its last restart (GiIntegrate), 0 when its history belongs to an
// older lighting epoch (a whole-cache restart).
template <typename B>
uint giRestartUpdates(B b, GiHeader h, uint entry)
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    return b.Load(a + GI_SH_EPOCH) == h.epoch && b.Load(a + GI_SH_HISTORY) != 0 ? (b.Load(a + GI_SH_RESTART) & 0xFFFFu) : 0u;
}
uint giPriorityBucket(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    if ((b.Load(GI_P1_FLAGS) & 1u) == 0) return giAgeBucket(b, h, entry);
    const uint n = giRestartUpdates(b, h, entry);
    if (n < 4) return GI_AGE_BUCKETS - 1 - n;
    if (n < 16) return GI_T0_BUCKET - 1 - (n - 4);
    const uint last = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE);
    return last == 0 ? GI_T1_BUCKET - 1 : min(h.frame - last, GI_T1_BUCKET - 1);
}
// The key of the cell containing a cell 'steps' levels up (s_{l+k} = 2^k s_l: cell / 2^k, floor), same normal class.
uint64_t giParentKey(uint64_t key, uint steps)
{
    const uint level = (uint)(key & 31u), normalClass = (uint)((key >> 5) & 7u);
    const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;  // sign-extend 18 bits
    return giKey(level + steps, normalClass, cell >> steps);
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
float3 giAnchorAtHit(GiHeader h, float3 hitPosition, float3 rayDirection, float rayDistance)
{
    // Only the traced segment is known to be free. Near a crease the desired
    // offset can exceed t and step behind the ray origin, through its surface.
    const float back = min(2e-3 + 4e-4 * distance(hitPosition, h.camera), 0.5 * max(rayDistance, 0.0));
    return hitPosition - rayDirection * back;
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

// Anchor resampling (gi.anchor_resample; redesign V2.2 P1'-b cause: the cached level depended on the history). An entry's
// value is the irradiance at its anchor, and the anchor was its creator's point (or the deterministic minimum of the
// first frame's candidates): a coarse cell over a lamp's 1 / d^2 gradient or next to an edge held the light of whichever
// point happened to come first, so the same view reached a different level after another history (bath, 7 % [measured]).
// Every lookup that finds an entry offers its surface point: a 64-bit maximum of (22-bit random priority | 42-bit point
// in the cell, giPackAnchorCandidate's grid), so each update's integration (GiIntegrate) moves the anchor to a uniform
// choice among the lookups since the entry's last update, and the history averages the irradiance over where the cell
// is read. The normal stays the entry's (a cell is one normal class). A plain load first: a lower priority adds no atomic.
#define GI_RESAMPLE_OFFSET 928  // header word: the candidates (8 B per entry), 0 = off
// Hit-cell direct-light accumulator (gi.hit_accumulator; RENDERER_REDESIGN_V2 12.1, P1''-a). The room's multi-bounce
// light is carried by the coarse hit cells, whose updates took the direct light of one point per texel ray - a lamp's
// 1/d^2 hotspot made them heavy-tailed, the running means walked, and the room's level differed between runs (SD 3.1 pp).
// Every GI hit adds its direct light to its cell's sums (A = sum of E_i, B = sum of E_i t_l,i, C = sum of E_i t_l,i
// returned_i / pi: the irradiance of the sun and the local light sample, E_i = n.l E vis, and the coat's light-side
// factors - HitShading's coat split), in 64-bit fixed point (integer sums: the same total in any order, deterministic);
// GiAccumulate folds each listed cell's frame sums into its means (samples-weighted, window gi.hit_accumulator_window
// samples, fewer while the sun changes). A hit whose cell holds gi.hit_accumulator_min_samples reads the means for its
// diffuse direct term: plain (1 - cover) keep_sheen(v) albedo A + cover t_v / eta^2 (albedo B + C), with its own albedo,
// normal and view (texture detail kept); the specular terms stay the point values. Unbiased in total over the cell's
// hits (the mean of the same population), the cell's light leaves it at the right level; per reader the hotspot moves
// within the cell (<= 2 texel cones).
#define GI_ACC_MODE 932         // bit 0: gi.hit_accumulator_frame (the rays' direct terms from this frame's cell means, GiAccFix),
                                // bit 1: energy audit (experiment 32768), bit 2: gi.hit_accumulator_ratio (response-weighted means)
#define GI_ACC_OFFSET 936       // header words: the means (48 B per entry: A, B, C float3, n float, epoch; 0 = off),
#define GI_ACC_SUMS 940         // the frame sums (GI_ACC_SUMS_BYTES per entry: per term T = A, B, C the numerator sum k_T T and
                                // the weight sum k_T, uint64 x 2^16 - A, B rgb, C's weight scalar - then the count),
#define GI_ACC_MIN 944          // the samples a cell needs before hits read it
#define GI_ACC_CELL_SCALE 948   // the accumulator's cell: this x the ray footprint (float; the bounce cell's scale: the bounce
                                // cell itself; smaller: its own finer entry, kept alive by the hits, never updated by rays)
#define GI_ACC_AUDIT 952        // energy audit (experiment 32768): 2 x uint64, sum of lum(point) and lum(accumulator) x 1024
#define GI_ACC_SCALE 65536.0
#define GI_ACC_SUMS_BYTES 144u  // 16 x uint64 (A: 3 + 3, B: 3 + 3, C: 3 + 1) + count, padded to 16 B
#define GI_ACC_COUNT 128u
// One hit's direct terms with the reader's factors k_T (D = k_A A + k_B B + k_C C): the cell's mean of a term is
// sum(k_T T) / sum(k_T), so that its readers' corrected values sum, over the frame's hits, to their point values exactly
// (gi.hit_accumulator_ratio; the plain mean sum(T) / n lost the energy where albedo and light vary together within a cell:
// bath audit 0.98). Without the ratio rule the weights are 1 (the plain mean).
void giAccRecord(RWByteAddressBuffer b, uint entry, float3 A, float3 B, float3 C, float3 kA, float3 kB, float kC)
{
    const uint base = b.Load(GI_ACC_SUMS) + entry * GI_ACC_SUMS_BYTES;
    if ((b.Load(GI_ACC_MODE) & 4u) == 0) kA = kB = 1, kC = 1;
    const float3 nA = kA * A, nB = kB * B, nC = kC * C;
    const float v[16] = { nA.x, nA.y, nA.z, kA.x, kA.y, kA.z, nB.x, nB.y, nB.z, kB.x, kB.y, kB.z, nC.x, nC.y, nC.z, kC };
    uint64_t prev;
    [unroll] for (uint k = 0; k < 16; ++k)
        if (v[k] > 0) b.InterlockedAdd64(base + k * 8, (uint64_t)(v[k] * GI_ACC_SCALE + 0.5), prev);
    uint prevCount;
    b.InterlockedAdd(base + GI_ACC_COUNT, 1u, prevCount);
}
bool giAccRead(RWByteAddressBuffer b, GiHeader h, uint entry, out float3 A, out float3 B, out float3 C)
{
    const uint a = b.Load(GI_ACC_OFFSET) + entry * 48;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32);
    A = asfloat(w0.xyz);
    B = asfloat(uint3(w0.w, w1.x, w1.y));
    C = asfloat(uint3(w1.z, w1.w, w2.x));
    return w2.z == h.epoch && asfloat(w2.y) >= (float)b.Load(GI_ACC_MIN);
}
void giAccClear(RWByteAddressBuffer b, uint entry)
{
    if (b.Load(GI_ACC_OFFSET) == 0) return;
    b.Store4(b.Load(GI_ACC_OFFSET) + entry * 48 + 32, uint4(0, 0, 0, 0));  // n = 0, epoch 0
    const uint s = b.Load(GI_ACC_SUMS) + entry * GI_ACC_SUMS_BYTES;
    [unroll] for (uint k = 0; k < GI_ACC_SUMS_BYTES / 16; ++k) b.Store4(s + k * 16, uint4(0, 0, 0, 0));
}
void giAnchorOffer(RWByteAddressBuffer b, GiHeader h, uint entry, uint64_t key, float3 p)
{
    const uint base = b.Load(GI_RESAMPLE_OFFSET);
    if (base == 0 || entry == GI_ENTRY_PENDING) return;
    const uint64_t spot = giPackAnchorCandidate(h, key, p, float3(0, 0, 1)) >> 22;  // 42 bits: the position
    const uint prio = (giDetHash((uint)spot ^ (uint)(spot >> 21) * 0x9E3779B9u ^ h.frame * 0x85EBCA6Bu) >> 10) | 1u;  // 22 bits, != 0
    const uint address = base + entry * 8;
    if ((b.Load(address + 4) >> 10) >= prio) return;  // (the high word holds the priority's 22 bits)
    uint64_t previous;
    b.InterlockedMax64(address, ((uint64_t)prio << 42) | spot, previous);
}
float3 giAnchorOfferPosition(GiHeader h, uint64_t key, uint64_t offer)
{
    const uint level = (uint)(key & 31u);
    const float s = giCellSize(h, level);
    const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;
    const uint3 q = uint3((uint)(offer >> 28) & 16383u, (uint)(offer >> 14) & 16383u, (uint)offer & 16383u);
    return float3(cell) * s - 0.5 * s + float3(q) / 16383.0 * (2 * s);
}

// Anchor centroid (gi.anchor_centroid; RENDERER_REDESIGN_V2 12.2, P1''-b): an entry's value is the irradiance at its anchor,
// and the anchor is the centroid of the surface points it is looked up at, not one point. Every lookup that finds the
// entry adds its point (giPackAnchorCandidate's 14-bit grid over the cell, integer sums: the same in any order) and keeps
// the lookup nearest the entry's current centroid (a 64-bit minimum of distance | point). Each update (GiIntegrate) folds
// the period's mean into an exponential mean over lookups (weight 1/64 each: 1 - (63/64)^n per update) and moves the
// anchor only when that mean left it by more than an eighth of the cell; a mean farther than that from every lookup
// (between two faces of a crease) moves it to the nearest lookup instead. A move halves the entry's running-mean count, so
// the new place's irradiance takes over within a few updates without a restart (no jump: the random resample's spots).
// 48 B per entry: sums x, y, z, count; nearest (64 bit); 8 B spare; centroid xyz, valid.
#define GI_CENTROID_OFFSET 968  // header word: the region (0 = off)
#define GI_CENTROID_MAX_COUNT 131072u  // lookups summed per update period (16383 x 2^17 < 2^32)
void giCentroidClear(RWByteAddressBuffer b, uint entry)
{
    const uint base = b.Load(GI_CENTROID_OFFSET);
    if (base == 0) return;
    b.Store4(base + entry * 48, uint4(0, 0, 0, 0));
    b.Store4(base + entry * 48 + 16, uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0));
    b.Store4(base + entry * 48 + 32, uint4(0, 0, 0, 0));
}
void giCentroidOffer(RWByteAddressBuffer b, GiHeader h, uint entry, uint64_t key, float3 p)
{
    const uint base = b.Load(GI_CENTROID_OFFSET);
    if (base == 0 || entry == GI_ENTRY_PENDING) return;
    const uint a = base + entry * 48;
    if (b.Load(a + 12) >= GI_CENTROID_MAX_COUNT) return;
    const uint64_t q = giPackAnchorCandidate(h, key, p, float3(0, 0, 1)) >> 22;  // 42 bits: 14 per axis
    uint previous;
    b.InterlockedAdd(a, (uint)(q >> 28) & 16383u, previous);
    b.InterlockedAdd(a + 4, (uint)(q >> 14) & 16383u, previous);
    b.InterlockedAdd(a + 8, (uint)q & 16383u, previous);
    b.InterlockedAdd(a + 12, 1u, previous);
    const float4 meanPoint = asfloat(b.Load4(a + 32));
    if (meanPoint.w > 0)
    {
        const float s = giCellSize(h, (uint)(key & 31u));
        const uint64_t d = (uint64_t)min(distance(p, meanPoint.xyz) / (4 * s) * 4194303.0, 4194303.0);  // 22 bits over 4 cells
        uint64_t previous64;
        b.InterlockedMin64(a + 16, (d << 42) | q, previous64);
    }
}

uint giFindOrCreate(RWByteAddressBuffer b, GiHeader h, uint64_t key, float3 anchor, float3 normal, out bool created)
{
    created = false;
    // Existing entries (nearly every call) by plain loads; the compare-exchange probe below only when the key is absent.
    const uint existing = giFind(b, h, key);
    if (existing != GI_ENTRY_PENDING)
    {
        giDetAnchorCandidate(b, h, existing, key, anchor, normal);
        giAnchorOffer(b, h, existing, key, anchor);
        giCentroidOffer(b, h, existing, key, anchor);
        return existing;
    }
    if ((h.flags & 1u) != 0)
    {
        // Missing keys are requests, never allocations in a reader dispatch.
        // GiAdmission sorts and deduplicates all requests before choosing the
        // keys that fit. Capacity covers every producer, not an append budget.
        uint request;
        b.InterlockedAdd(h.offAdmission, 1u, request);
        const uint limit = b.Load(h.offAdmission + 4);
        if (request >= limit)
        {
            b.InterlockedOr(h.offAdmission + 24, 1u); // hard diagnostic: a producer-bound violation
            b.InterlockedAdd(GI_H_STAT_TABLE_FULL, 1u); // exposed by the existing GI statistics/readback
            return GI_ENTRY_PENDING;
        }
        const uint a = h.offAdmission + 256 + (h.capacity + request) * 32;
        const uint64_t candidate = giPackAnchorCandidate(h, key, anchor, normal);
        b.Store4(a, uint4((uint)key, (uint)(key >> 32), (uint)candidate, (uint)(candidate >> 32)));
        b.Store4(a + 16, uint4(0, 1, 0, 0)); // entry unused, kind = new
        return GI_ENTRY_PENDING;
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
            if (b.Load(GI_RESAMPLE_OFFSET) != 0) b.Store2(b.Load(GI_RESAMPLE_OFFSET) + entry * 8, uint2(0, 0));  // no offer of a previous occupant
            giAccClear(b, entry);  // (gi.hit_accumulator: not a previous occupant's light)
            giCentroidClear(b, entry);  // (gi.anchor_centroid: nor its lookups)
            if (h.flags & 1u)
            {
                b.Store2(h.offAnchorMin + entry * 8, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
                uint64_t previous;
                b.InterlockedMin64(h.offAnchorMin + entry * 8, giPackAnchorCandidate(h, key, anchor, normal), previous);
            }
            // The new entry's record (zero updates) before the entry is published: a reader that finds the key sees either
            // GI_ENTRY_PENDING or a record without updates, never the previous occupant's (readers run concurrently in the
            // same dispatch, and the main view's screen lookups beside the reflection hits, GiSystem::recordScreen); in
            // deterministic mode also the candidate above.
            DeviceMemoryBarrier();
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
