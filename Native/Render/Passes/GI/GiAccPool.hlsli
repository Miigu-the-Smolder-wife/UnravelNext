// Hit direct-light accumulator, pool form (gi.hit_accumulator_pool; RENDERER_REDESIGN_V2 12.8, V2.3: the estimator after
// P1''-a's measurements). A ray hit's direct light is the light of one point; where a lamp's 1 / d^2 hotspot lies in the
// ray's footprint, the point value is heavy-tailed (the wall blotches and the floor sparkle of the game scenes). The
// accumulator replaces the hit's diffuse direct term by the mean of that term over the surface cell the footprint covers,
// measured by all the rays that hit the cell over the last frames.
//
// API (readers and writers; R owns this file):
//   struct GiAccMeans { float3 A, B, C; float weight; float cellSize; };
//   bool giAccPoolRead(pool, position, normal, footprint, out GiAccMeans m)
//       pool: the accumulator buffer (FrameResources::giAccumulator; ByteAddressBuffer or RWByteAddressBuffer).
//       footprint: the ray's footprint width at the hit (m). The finest cell is GI_ACCP fineScale x footprint; the read
//       takes the finest of 4 levels (x 1, 2, 4, 8 of that cell) whose window holds at least minSamples hits.
//       m.A, m.B, m.C: the cell's means of the light-side terms (HitShading's coat split: A = sum E_i, B = sum E_i t_l,i,
//       C = sum E_i t_l,i returned_i / pi); the reader's diffuse direct radiance is kA m.A + kB m.B + kC m.C with its
//       own factors (kA = plain x albedo / pi, kB = coated x albedo / pi, kC = coated).
//       m.weight: 1, or n / minSamples when even the coarsest level holds fewer hits (a continuous hand-over):
//       radiance += m.weight x (kA m.A + kB m.B + kC m.C - point value).
//       m.cellSize: the edge of the cell read (m) - a reader whose footprint is far below it may keep its point value.
//       false: no level has data (keep the point value).
//   void giAccPoolRecord(pool (RW), position, normal, footprint, A, B, C, kA, kB, kC)
//       one hit's terms and its own factors, into the finest cell's frame sums.
// Frame order: r.gi.trace (records; reads the window of the frames before) -> r.gi.acc.fold0..3 (GiAccFold.hlsl: each
// touched cell folds its frame sums into its window and adds them to its parent's frame sums) -> later readers (reflection
// hits) read the window including this frame.
// Who records: the GI rays. A ratio estimator keeps energy, sum over hits of k R = sum of k T, for the population of
// hits that formed R; hits of another population (reflection rays: other positions within the cell, other factors) may
// read R but would shift it for the GI rays if they recorded (design 12.1-1 lists both: to be decided with the audit).
//
// Estimator (12.8 (1), window ratio): per cell and term, numerator N = sum over frames w_f sum over hits k T and
// denominator D = sum w_f sum k, the same frame weights w_f = (1 - alpha)^age for both; the reader takes N / D. In the
// steady state its expectation keeps energy (E[sum k] E[k T] / E[k] = E[sum k T]); the ratio's own bias is O(1 / n).
// No per-frame correction (GiAccFix put each frame's noise back). Restart: when the frame's ratio R_f leaves the window's
// by more than 3 standard errors of the frame, sigma_f = sqrt(sum k^2 (T - R)^2) / sum k (luminance of term A, from the
// frame's moments), the window restarts at the frame's sums (a light or the readers changed); also on a new lighting epoch.
// Levels (12.8 (2)): hits write the finest level only; r.gi.acc.fold adds a cell's frame sums to its parent (exact:
// sums), so four levels hold N, D over cells of x 1, 2, 4, 8; the reader's bias is the width of the level it reads, its
// variance is bounded by minSamples.
//
// Buffer (raw), S = slots (a power of two):
//   header 64 B: { S, frame, lighting epoch, exposure scale (float) }, { keep = 1 - alpha (float), minSamples (float),
//     fineScale (float), GI cellSize0 (float) }, { 0, hits left out (statistics), 0, 0 }, { touched count of list 0..3 }
//     (bytes 48-63)
//   keys      at 64:        S x 8 B (0 = empty; open addressing, GI_ACCP_PROBES linear probes, a lookup reads them all:
//                           eviction empties slots, so an empty slot does not end a search)
//   stamps    at 64 + 8 S:  S x 4 B, frame x 4 + list + 1 of the slot's last touch (0 = never): the first toucher of a
//                           list in a frame appends the slot to it (a cell hit by rays and fed by finer cells is in
//                           list 0 and in a later one: each fold takes what has arrived since its last)
//   lists     at 64 + 12 S: 4 x S x 4 B, the slots touched this frame per level step (0: by hits, k: by fold k - 1)
//   payload   at 64 + 28 S: S x 240 B:
//     +0   frame sums, 19 x uint64 fixed point: [0..2] kA A, [3..5] kA, [6..8] kB B, [9..11] kB, [12..14] kC C, [15] kC
//          (x 2^16); [16] sum (e l)^2, [17] sum k_s e l, [18] sum k_s^2 (x 2^24; l = lum(kA A), k_s = lum(kA), e = the
//          exposure scale: the restart test's moments in display-referred units, so the fixed point holds lit and dark
//          cells alike)
//     +152 frame hit count, +156 0
//     +160 window: { n (float, weighted hits), frame of the last fold, lighting epoch, 0 }
//     +176 window sums, 16 floats in the order of [0..15]
#ifndef UNX_GI_ACCPOOL_HLSLI
#define UNX_GI_ACCPOOL_HLSLI
#include "Passes/GI/GiCache.hlsli"

#define GI_ACCP_HEADER 64u
#define GI_ACCP_PROBES 16u
#define GI_ACCP_LEVELS 4u
#define GI_ACCP_PAYLOAD 240u
#define GI_ACCP_SUMS 19u
#define GI_ACCP_COUNT 152u
#define GI_ACCP_WINDOW 160u
#define GI_ACCP_WINDOW_SUMS 176u
#define GI_ACCP_SCALE 65536.0
#define GI_ACCP_MOMENT_SCALE 16777216.0
#define GI_ACCP_TOP_LEVEL 27u  // the finest cells' level is at most this (4 levels above it fit the key's 5 bits)
#define GI_ACCP_NONE 0xFFFFFFFFu

struct GiAccPoolHeader
{
    uint slots, frame, epoch;
    float exposure, keep, minSamples, fineScale, cellSize0;
};
struct GiAccMeans
{
    float3 A, B, C;
    float weight;
    float cellSize;
};

template <typename B>
GiAccPoolHeader giAccPoolHeader(B pool)
{
    const uint4 a = pool.Load4(0), b = pool.Load4(16);
    GiAccPoolHeader h;
    h.slots = a.x;
    h.frame = a.y;
    h.epoch = a.z;
    h.exposure = asfloat(a.w);
    h.keep = asfloat(b.x);
    h.minSamples = asfloat(b.y);
    h.fineScale = asfloat(b.z);
    h.cellSize0 = asfloat(b.w);
    return h;
}
uint giAccpKeyAddress(GiAccPoolHeader h, uint slot) { return GI_ACCP_HEADER + slot * 8; }
uint giAccpStampAddress(GiAccPoolHeader h, uint slot) { return GI_ACCP_HEADER + h.slots * 8 + slot * 4; }
uint giAccpListAddress(GiAccPoolHeader h, uint list, uint index) { return GI_ACCP_HEADER + h.slots * 12 + (list * h.slots + index) * 4; }
uint giAccpPayload(GiAccPoolHeader h, uint slot) { return GI_ACCP_HEADER + h.slots * 28 + slot * GI_ACCP_PAYLOAD; }

float giAccpCellSize(GiAccPoolHeader h, uint level) { return h.cellSize0 * exp2((float)level); }
// The finest cell's level of a hit whose ray footprint is 'footprint' wide.
uint giAccpFineLevel(GiAccPoolHeader h, float footprint)
{
    const float size = max(h.fineScale * footprint, h.cellSize0);
    return min((uint)max(ceil(log2(size / h.cellSize0) - 1e-4), 0.0), GI_ACCP_TOP_LEVEL);
}
// Key: level (5) | normal class (3) | cell x, y, z (18 bits each, wrapping); bit 63 set (never 0). Cells nest: a level's
// cell (x, y, z) lies in the next level's (x >> 1, y >> 1, z >> 1).
uint64_t giAccpKey(uint level, uint normalClass, int3 cell)
{
    const uint3 c = uint3(cell) & 0x3FFFFu;
    return (uint64_t)level | ((uint64_t)normalClass << 5) | ((uint64_t)c.x << 8) | ((uint64_t)c.y << 26) | ((uint64_t)c.z << 44) | (1ull << 63);
}
uint64_t giAccpKeyAt(GiAccPoolHeader h, float3 position, float3 normal, uint level)
{
    return giAccpKey(level, giNormalClass(normal), int3(floor(position / giAccpCellSize(h, level))));
}
uint64_t giAccpParentKey(uint64_t key)
{
    const uint level = (uint)(key & 31u), nc = (uint)(key >> 5) & 7u;
    const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;
    return giAccpKey(level + 1, nc, cell >> 1);
}
template <typename B>
uint64_t giAccpLoadKey(B pool, uint address)
{
    const uint2 w = pool.Load2(address);
    return ((uint64_t)w.y << 32) | w.x;
}
// The key's slot, or GI_ACCP_NONE. Every probe is read (an evicted slot is empty and does not end the search).
template <typename B>
uint giAccpFind(B pool, GiAccPoolHeader h, uint64_t key)
{
    const uint first = giHash(key);
    uint found = GI_ACCP_NONE;
    [loop] for (uint i = 0; i < GI_ACCP_PROBES && found == GI_ACCP_NONE; ++i)
    {
        const uint slot = (first + i) & (h.slots - 1);
        if (giAccpLoadKey(pool, giAccpKeyAddress(h, slot)) == key) found = slot;
    }
    return found;
}
// The key's slot, taking the first empty probe when it has none; GI_ACCP_NONE when all its probes hold other keys (the
// caller then leaves the hit out: its point value stands). Two creators of one key meet at the same first empty slot:
// the loser of the exchange finds the key there.
uint giAccpFindOrCreate(RWByteAddressBuffer pool, GiAccPoolHeader h, uint64_t key)
{
    const uint first = giHash(key);
    uint found = GI_ACCP_NONE, empty = GI_ACCP_PROBES;
    [loop] for (uint i = 0; i < GI_ACCP_PROBES && found == GI_ACCP_NONE; ++i)
    {
        const uint slot = (first + i) & (h.slots - 1);
        const uint64_t k = giAccpLoadKey(pool, giAccpKeyAddress(h, slot));
        if (k == key) found = slot;
        else if (k == 0 && empty == GI_ACCP_PROBES) empty = i;
    }
    [loop] for (uint j = empty; j < GI_ACCP_PROBES && found == GI_ACCP_NONE; ++j)
    {
        const uint slot = (first + j) & (h.slots - 1);
        uint64_t previous;
        pool.InterlockedCompareExchange64(giAccpKeyAddress(h, slot), 0ull, key, previous);
        if (previous == 0 || previous == key) found = slot;
    }
    return found;
}
// Marks the slot touched in this frame's list; the first toucher appends it to the list.
uint giAccpStamp(uint frame, uint list) { return frame * GI_ACCP_LEVELS + list + 1; }
void giAccpTouch(RWByteAddressBuffer pool, GiAccPoolHeader h, uint slot, uint list)
{
    const uint address = giAccpStampAddress(h, slot);
    const uint stamp = giAccpStamp(h.frame, list);
    if (pool.Load(address) == stamp) return;
    uint previous;
    pool.InterlockedExchange(address, stamp, previous);
    if (previous == stamp) return;
    uint index;
    pool.InterlockedAdd(48 + list * 4, 1u, index);
    if (index < h.slots) pool.Store(giAccpListAddress(h, list, index), slot);
}

void giAccPoolRecord(RWByteAddressBuffer pool, float3 position, float3 normal, float footprint, float3 A, float3 B, float3 C, float3 kA, float3 kB, float kC)
{
    const GiAccPoolHeader h = giAccPoolHeader(pool);
    const uint slot = giAccpFindOrCreate(pool, h, giAccpKeyAt(h, position, normal, giAccpFineLevel(h, footprint)));
    if (slot == GI_ACCP_NONE)
    {
        uint failed;
        pool.InterlockedAdd(36, 1u, failed);  // statistics: hits left out (all probes taken)
        return;
    }
    const uint base = giAccpPayload(h, slot);
    const float3 nA = kA * A, nB = kB * B, nC = kC * C;
    const float v[16] = { nA.x, nA.y, nA.z, kA.x, kA.y, kA.z, nB.x, nB.y, nB.z, kB.x, kB.y, kB.z, nC.x, nC.y, nC.z, kC };
    uint64_t previous;
    [unroll] for (uint k = 0; k < 16; ++k)
        if (v[k] > 0) pool.InterlockedAdd64(base + k * 8, (uint64_t)(min(v[k], 1.0e12) * GI_ACCP_SCALE + 0.5), previous);
    // the restart test's moments of term A (luminance, exposure-scaled)
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    const float ks = dot(kA, Y), el = min(dot(nA, Y) * h.exposure, 1.0e3);
    if (ks > 0)
    {
        pool.InterlockedAdd64(base + 16 * 8, (uint64_t)(el * el * GI_ACCP_MOMENT_SCALE + 0.5), previous);
        pool.InterlockedAdd64(base + 17 * 8, (uint64_t)(ks * el * GI_ACCP_MOMENT_SCALE + 0.5), previous);
        pool.InterlockedAdd64(base + 18 * 8, (uint64_t)(ks * ks * GI_ACCP_MOMENT_SCALE + 0.5), previous);
    }
    uint count;
    pool.InterlockedAdd(base + GI_ACCP_COUNT, 1u, count);
    giAccpTouch(pool, h, slot, 0);
}

template <typename B>
bool giAccPoolRead(B pool, float3 position, float3 normal, float footprint, out GiAccMeans m)
{
    m.A = m.B = m.C = 0;
    m.weight = 0;
    m.cellSize = 0;
    const GiAccPoolHeader h = giAccPoolHeader(pool);
    const uint fine = giAccpFineLevel(h, footprint);
    uint chosen = GI_ACCP_NONE, chosenLevel = 0;
    float chosenSamples = 0;
    [loop] for (uint k = 0; k < GI_ACCP_LEVELS && !(chosenSamples >= h.minSamples); ++k)
    {
        const uint slot = giAccpFind(pool, h, giAccpKeyAt(h, position, normal, fine + k));
        if (slot == GI_ACCP_NONE) continue;
        const uint4 w = pool.Load4(giAccpPayload(h, slot) + GI_ACCP_WINDOW);  // n, frame of the last fold, epoch
        if (w.z != h.epoch) continue;
        // the window as a reader sees it now: the weights of its frames have aged since the last fold
        const float samples = asfloat(w.x) * pow(h.keep, (float)min(h.frame - w.y, 4096u));
        if (!(samples > 0)) continue;
        // (the finest level with enough hits ends the search; below that the coarsest level with any is kept)
        chosen = slot;
        chosenLevel = fine + k;
        chosenSamples = samples;
    }
    if (chosen == GI_ACCP_NONE) return false;
    const uint a = giAccpPayload(h, chosen) + GI_ACCP_WINDOW_SUMS;
    const float4 s0 = asfloat(pool.Load4(a)), s1 = asfloat(pool.Load4(a + 16)), s2 = asfloat(pool.Load4(a + 32)), s3 = asfloat(pool.Load4(a + 48));
    const float3 dA = float3(s0.w, s1.x, s1.y), dB = float3(s2.y, s2.z, s2.w);
    // a term no hit weighed (its factor 0 for every hit: no reader uses it) is 0
    m.A = float3(dA.x > 0 ? s0.x / dA.x : 0.0, dA.y > 0 ? s0.y / dA.y : 0.0, dA.z > 0 ? s0.z / dA.z : 0.0);
    m.B = float3(dB.x > 0 ? s1.z / dB.x : 0.0, dB.y > 0 ? s1.w / dB.y : 0.0, dB.z > 0 ? s2.x / dB.z : 0.0);
    m.C = s3.w > 0 ? s3.xyz / s3.w : float3(0, 0, 0);
    m.weight = saturate(chosenSamples / max(h.minSamples, 1.0));
    m.cellSize = giAccpCellSize(h, chosenLevel);
    return true;
}
#endif
