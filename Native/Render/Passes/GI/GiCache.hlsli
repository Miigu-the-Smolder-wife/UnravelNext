// World radiance cache (R track, ARCHITECTURE 2.5; INTERFACES 5.6 public API: GiSrvs, giCacheIrradiance,
// giCacheRadiance). Consumers: M (planar-reflection views, which have no screen probes), R's own kernels.
//
// Structure (all in one raw buffer, FrameResources::giCache; offsets in its 256 B header):
//  - Entries live on surfaces, keyed by (level, cell, normal class). Level of a screen surface: the cell edge grows with
//    the distance to the main camera so a cell subtends ~gi.cache_cell_angle_deg (s_L = s0 * 2^L). Level of a ray hit:
//    at least the ray's footprint there (texel cone, ~0.36 t), so off-screen bounce geometry gets few coarse entries.
//    Normal class = dominant axis of the surface normal (+-x, +-y, +-z): the two sides of a thin wall never share.
//  - Hash table: 64-bit keys, linear probing, rebuilt from the live entries every frame (no tombstones).
//  - Per entry: anchor (surface point + normal where its rays start), 8 x 8 hemispherical octahedral incident radiance
//    texels (RGB + hit distance, fp16) and their cosine-convolved L2 spherical-harmonic irradiance (world frame, fp16),
//    the anchor's sun visibility, and update bookkeeping. An update traces all 64 texels (GiTrace), then projects them
//    with exact per-texel SH integrals and an exact L2 rotation to world (GiIntegrate). Multi-bounce: ray hits read the
//    hit cell's irradiance (Jacobi iteration over updates; no recursion).
//  - Radiance here is indirect + sky; the sun's direct light is not in the irradiance (M adds it with the VSM). The sun
//    term appears only in the radiance leaving ray hits (bounce light), with the hit cell's cached sun visibility.
#ifndef UNX_GI_CACHE_HLSLI
#define UNX_GI_CACHE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

struct GiSrvs
{
    uint cache, hash, pad0, pad1;  // cache: SRV (raw) of FrameResources::giCache; hash: the same SRV (one buffer)
};

#define GI_PI 3.14159265358979
#define GI_TEXELS 8u
#define GI_TEXEL_COUNT 64u
#define GI_ENTRY_PENDING 0xFFFFFFFFu
#define GI_PROBE_LIMIT 32u  // linear probing bound (the table is kept at <= 50 % load)
// Stored radiometric values (texels, SH, probe records) are fp16 of value * GI_STORE_SCALE: daylight irradiance SH
// (DC coefficient ~3.5 x 3e4 lux) would overflow fp16 unscaled; scaled, the range is ~4e6 with normal precision down
// to 0.004 nit.
#define GI_STORE_SCALE (1.0 / 64.0)
#define GI_LOAD_SCALE 64.0

// Entry SH block (80 B): words 0..13 = 27 fp16 irradiance coefficients (x GI_STORE_SCALE) + fp16 sun visibility (high
// half of word 13), then bookkeeping words.
#define GI_SH_STRIDE 80u
#define GI_SH_UPDATES 56u      // completed updates (all time)
#define GI_SH_SUN_SAMPLES 60u  // sun visibility samples
#define GI_SH_LAST_UPDATE 64u  // frame stamp of the last selection/update
#define GI_SH_HISTORY 68u      // convergence phase | mean samples << 12 | Jacobi length / 16 << 24 (GiInternal giHistoryAlpha)
#define GI_SH_EPOCH 72u        // lighting epoch of that history
#define GI_SH_WINDOW 76u       // GiIntegrate's window test: fast mean of the anchor irradiance | mean of its per-update sample
                               // spread (sigma) << 16, fp16 x GI_STORE_SCALE

// Entry irradiance map (design 2.5 revision, request 18): E(n) at the 9 x 9 hemispherical octahedral directions around
// the entry's anchor normal (texel (i, j) = giHemiOctDecode(((i, j) + 0.5) / 9); the pole, the anchor normal itself, is
// the centre texel), estimated per ray: every update's 64 rays add L max(0, n_j . w) dw, dw = the ray's solid-angle
// weight (2 / |p|^3 x the texel's UV area for the hemispherical octahedral map). No texel averaging (a thin band of
// horizon light is not smeared over a texel) and no SH truncation. RGB9E5 x GI_STORE_SCALE, 81 words + 3 padding.
// Evaluated with Catmull-Rom (16 texels), clamped at 0.
#define GI_IRR_N 9u
#define GI_IRR_STRIDE 336u
#define GI_IRR_POLE 40u  // the centre texel (4, 4): the anchor normal

// Header (uint4 rows of the first 256 B).
struct GiHeader
{
    uint capacity, tableSlots;
    float cellSize0, cellTan;         // s0 (m), tan(cell angle)
    uint offTable, offFree, offMeta, offAnchor;
    uint offSh, offTexels, offUpdate, maxAge;
    uint freeCount, updateCount, selectedCount, frame;
    uint backgroundCursor, backgroundCount, epoch, liveCount;
    float3 camera;                    // main camera position of this frame (level selection for every view)
    uint maxLevel;
    uint offSelected, offHitStamp, offHitList, offShTable;
    uint hitCount0, hitCount1, jacobiUpdates, historyMax, historyStatic;  // row 7 .w: historyMax | historyStatic << 16
    uint offAnchorMin, flags;         // deterministic anchors (per entry 64-bit min of packed candidates); flags bit 0 = gi.deterministic
    uint offIrr;                      // irradiance maps (GI_IRR_STRIDE per entry)
    uint offSlotAnchor;               // deterministic anchors: per table slot 64-bit min of the candidates of threads that
                                      // found the key with its entry not yet published (GiDetFold)
};

template <typename B>
GiHeader giHeader(B b)
{
    GiHeader h;
    const uint4 r0 = b.Load4(0), r1 = b.Load4(16), r2 = b.Load4(32), r3 = b.Load4(48), r4 = b.Load4(64), r5 = b.Load4(80), r6 = b.Load4(96), r7 = b.Load4(112);
    h.capacity = r0.x; h.tableSlots = r0.y; h.cellSize0 = asfloat(r0.z); h.cellTan = asfloat(r0.w);
    h.offTable = r1.x; h.offFree = r1.y; h.offMeta = r1.z; h.offAnchor = r1.w;
    h.offSh = r2.x; h.offTexels = r2.y; h.offUpdate = r2.z; h.maxAge = r2.w;
    h.freeCount = r3.x; h.updateCount = r3.y; h.selectedCount = r3.z; h.frame = r3.w;
    h.backgroundCursor = r4.x; h.backgroundCount = r4.y; h.epoch = r4.z; h.liveCount = r4.w;
    h.camera = asfloat(r5.xyz); h.maxLevel = r5.w;
    h.offSelected = r6.x; h.offHitStamp = r6.y; h.offHitList = r6.z; h.offShTable = r6.w;
    h.hitCount0 = r7.x; h.hitCount1 = r7.y; h.jacobiUpdates = r7.z; h.historyMax = r7.w & 0xFFFFu; h.historyStatic = r7.w >> 16;
    const uint4 r15 = b.Load4(240);
    h.offAnchorMin = r15.x; h.flags = r15.y; h.offIrr = r15.z; h.offSlotAnchor = r15.w;
    return h;
}

// ---- keys

uint giNormalClass(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x >= 0 ? 0u : 1u;
    if (a.y >= a.z) return n.y >= 0 ? 2u : 3u;
    return n.z >= 0 ? 4u : 5u;
}

uint giLevelForSize(GiHeader h, float size) { return min((uint)max(ceil(log2(max(size, h.cellSize0) / h.cellSize0) - 1e-4), 0.0), h.maxLevel); }
uint giLevel(GiHeader h, float3 p) { return giLevelForSize(h, distance(p, h.camera) * h.cellTan); }
float giCellSize(GiHeader h, uint level) { return h.cellSize0 * exp2((float)level); }

// 62-bit key: level (5) | normal class (3) | cell x, y, z (18 bits each, wrapping) ; bit 63 set so a key is never 0.
uint64_t giKey(uint level, uint normalClass, int3 cell)
{
    const uint3 c = uint3(cell) & 0x3FFFFu;
    const uint64_t k = (uint64_t)level | ((uint64_t)normalClass << 5) | ((uint64_t)c.x << 8) | ((uint64_t)c.y << 26) | ((uint64_t)c.z << 44);
    return k | (1ull << 63);
}

uint giHash(uint64_t key)
{
    uint64_t x = key;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return (uint)x;
}

// Table slot: uint64 key at +0, entry index at +8 (16 B per slot).
template <typename B>
uint giFind(B b, GiHeader h, uint64_t key)
{
    uint slot = giHash(key) & (h.tableSlots - 1);
    [loop] for (uint i = 0; i < GI_PROBE_LIMIT; ++i)
    {
        const uint4 s = b.Load4(h.offTable + slot * 16);
        const uint64_t k = (uint64_t)s.x | ((uint64_t)s.y << 32);
        if (k == key) return s.z;  // GI_ENTRY_PENDING while its creator is still writing it
        if (k == 0) return GI_ENTRY_PENDING;
        slot = (slot + 1) & (h.tableSlots - 1);
    }
    return GI_ENTRY_PENDING;
}

// ---- texel mapping: hemispherical octahedral 8 x 8 around the entry normal

void giBasis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// Local hemisphere direction (z >= 0) <-> [0,1]^2.
float2 giHemiOctEncode(float3 d)
{
    const float3 v = d / (abs(d.x) + abs(d.y) + max(d.z, 0.0));
    return float2(v.x + v.y, v.x - v.y) * 0.5 + 0.5;
}

float3 giHemiOctDecode(float2 uv)
{
    const float2 q = uv * 2 - 1;
    const float2 p = float2(q.x + q.y, q.x - q.y) * 0.5;
    return normalize(float3(p, 1 - abs(p.x) - abs(p.y)));
}

// ---- spherical harmonics (real, order 2; irradiance-convolved coefficients, Ramamoorthi & Hanrahan 2001)

void giShBasis(float3 d, out float y[9])
{
    y[0] = 0.282095;
    y[1] = 0.488603 * d.y;
    y[2] = 0.488603 * d.z;
    y[3] = 0.488603 * d.x;
    y[4] = 1.092548 * d.x * d.y;
    y[5] = 1.092548 * d.y * d.z;
    y[6] = 0.315392 * (3 * d.z * d.z - 1);
    y[7] = 1.092548 * d.x * d.z;
    y[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

float3 giIrrUnpack(uint v)
{
    const float scale = asfloat(((v >> 27) + 103u) << 23);  // 2^(e - 24), bias 15, 9-bit mantissa
    return float3(v & 0x1FFu, (v >> 9) & 0x1FFu, (v >> 18) & 0x1FFu) * scale;
}

// Catmull-Rom weights for the texels at offsets -1, 0, 1, 2 of a fraction t.
float4 giCatmullRom(float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

// Anchor normal: octahedral snorm16 x 2 (the anchor record's fourth word).
float3 giUnpackAnchorNormal(uint packed)
{
    const float2 e = float2(int2(packed << 16, packed) >> 16) / 32767.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

// Visibility of a lookup point from an entry's anchor (gi.anchor_visibility, GiHeader.flags bit 1). An entry holds the
// light arriving at its anchor; a lookup point in the same cell that the anchor's rays cannot reach - the other side of a
// wall, when a room's floor and the ground outside share a coarse cell and normal class - read the outside's light (the
// ~3 % sunlight through walls, R_STATUS). The entry's texel toward the point holds the running mean distance its rays
// travelled before a hit (GiIntegrate, texel half 3): a point farther than 1.5 x that plus a quarter cell, or well behind
// the anchor's surface (more than 45 deg below its tangent plane), is not seen. Points within a quarter cell of the
// anchor are seen. A rejected corner's weight goes to the seen corners of its own level (giScreenVisRenorm; the ray-hit
// lookups normalise within the level), never to the next coarser level, whose larger cells reach through the wall.
// Visibility is judged only in a level whose every corner with data is converged (GI_VIS_MIN_UPDATES updates: the texel
// distance a mean of that many rays); a level with a young corner is read as without the test (a young entry's one- or
// two-ray distance rejects at random, and a rejected converged corner would hand its weight to still-converging ones).
// The leak is between long-lived cells (a room's floor and the ground outside). A level whose every corner with data is
// not seen keeps them all (visibility never empties a level), and every corner read is kept alive and requested as
// before (giKeepRead).
#define GI_VIS_MIN_UPDATES 16u
// Whether visibility is judged for a corner entry with 'updates' updates (the level is judged when all its corners are).
bool giVisJudged(GiHeader h, uint updates) { return (h.flags & 2u) != 0 && updates >= GI_VIS_MIN_UPDATES; }
// (anchor: the entry's anchor position, its word at offAnchor, read by the caller with the normal in one load)
template <typename B>
bool giAnchorSeesPointAt(B b, GiHeader h, uint entry, float3 anchor, float3 anchorNormal, float3 p, float cellSize)
{
    if ((h.flags & 2u) == 0) return true;
    const float3 d = p - anchor;
    const float dist = length(d), slack = 0.25 * cellSize;
    if (!(dist > slack)) return true;
    const float3 dir = d / dist;
    const float up = dot(dir, anchorNormal);
    if (up < -0.7071) return false;
    float3 t, bt;
    giBasis(anchorNormal, t, bt);
    float3 local = float3(dot(dir, t), dot(dir, bt), max(up, 0.0));
    local = dot(local, local) > 1e-12 ? normalize(local) : float3(0, 0, 1);
    const uint2 tx = min(uint2(giHemiOctEncode(local) * GI_TEXELS), uint2(GI_TEXELS - 1, GI_TEXELS - 1));
    const float reach = f16tof32(b.Load(h.offTexels + (entry * GI_TEXEL_COUNT + tx.y * GI_TEXELS + tx.x) * 8 + 4) >> 16);
    return dist <= 1.5 * reach + slack;
}
template <typename B>
bool giAnchorSeesPoint(B b, GiHeader h, uint entry, float3 anchorNormal, float3 p, float cellSize)
{
    return giAnchorSeesPointAt(b, h, entry, asfloat(b.Load3(h.offAnchor + entry * 16)), anchorNormal, p, cellSize);
}

// Irradiance (x 1, not stored scale) of an entry for normal n from its map; normals below the entry's hemisphere use
// its horizon.
template <typename B>
float3 giIrrMapAt(B b, GiHeader h, uint entry, float3 na, float3 n)
{
    float3 t, bt;
    giBasis(na, t, bt);
    float3 local = float3(dot(n, t), dot(n, bt), max(dot(n, na), 0.0));
    local = dot(local, local) > 1e-12 ? normalize(local) : float3(0, 0, 1);
    const float2 e = giHemiOctEncode(local) * (float)GI_IRR_N - 0.5;
    const int2 i0 = int2(floor(e));
    const float2 f = e - float2(i0);
    const float4 wx = giCatmullRom(f.x), wy = giCatmullRom(f.y);
    const uint base = h.offIrr + entry * GI_IRR_STRIDE;
    // A row's four texels are consecutive words: one Load4 per row from x0 = clamp(i0.x - 1, 0, N - 4), which holds every
    // clamped column the row reads (i0.x - 1 is in [-2, 7]: the clamped columns stay within x0 .. x0 + 3). The same
    // texels, weights and order as one load per texel (bit-identical), a quarter of the load instructions: the map
    // evaluation is the bulk of every cache lookup (M's per-pixel irradiance, ray hits).
    const int x0 = clamp(i0.x - 1, 0, (int)GI_IRR_N - 4);
    float3 sum = 0;
    [unroll] for (uint jy = 0; jy < 4; ++jy)
    {
        const uint iy = (uint)clamp(i0.y - 1 + (int)jy, 0, (int)GI_IRR_N - 1);
        const uint4 words = b.Load4(base + (iy * GI_IRR_N + (uint)x0) * 4);
        float3 row = 0;
        [unroll] for (uint jx = 0; jx < 4; ++jx)
        {
            const int k = clamp(i0.x - 1 + (int)jx, 0, (int)GI_IRR_N - 1) - x0;  // in [0, 3]
            row += wx[jx] * giIrrUnpack(k == 0 ? words.x : (k == 1 ? words.y : (k == 2 ? words.z : words.w)));
        }
        sum += wy[jy] * row;
    }
    return max(sum, 0.0) * GI_LOAD_SCALE;
}
// giIrrMapAt with the entry's anchor normal read here.
template <typename B>
float3 giIrrMap(B b, GiHeader h, uint entry, float3 n) { return giIrrMapAt(b, h, entry, giAnchorNormal(b, h, entry), n); }

template <typename B>
float3 giShIrradiance(B b, GiHeader h, uint entry, float3 n, out float sunVisibility)
{
    // The entry's irradiance map (per-ray estimate, exact at its grid directions); the SH block keeps the sun visibility.
    sunVisibility = f16tof32(b.Load(h.offSh + entry * GI_SH_STRIDE + 52) >> 16);
    return giIrrMap(b, h, entry, n);
}

// Irradiance (indirect + sky, no direct sun) at a surface point: trilinear over the 8 cells of the point's level (at least
// minLevel) and normal class, entries that exist and have been updated at least once (renormalised); coarser levels when
// none does (up to GI_LEVEL_CLIMB more: points no probe sees only have the coarse cells GI rays created there), then finer
// ones down to the point's own level (giCacheLevels).
#define GI_LEVEL_CLIMB 6u

// Read hook of the lookups below, per contributing entry. Read-only readers (M's shading) do nothing; R's ray hits read
// through the RW cache and keep what they read alive and requested (GiInternal.hlsli): an entry only readers see must not
// be left at its first, unconverged update. w = the entry's trilinear weight in the lookup.
void giKeepRead(ByteAddressBuffer b, GiHeader h, uint entry, float w) {}
void giKeepRead(RWByteAddressBuffer b, GiHeader h, uint entry, float w);

// One level's trilinear accumulation over the 8 cells of the point (entries that exist and have been updated).
template <typename B>
void giAccumulateLevel(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, bool wantRadiance, uint nc, uint level, inout float3 sumE,
                       inout float3 sumL, inout float weight, float cone = 0, bool emitters = false)
{
    const float s = giCellSize(h, level);
    const float3 f = worldPos / s - 0.5;
    const int3 c0 = int3(floor(f));
    const float3 t = f - floor(f);
    float3 hiddenE = 0, hiddenL = 0;  // the corners the visibility test left out (added when it left out all of them)
    float hiddenW = 0, seenW = 0;
    bool young = false;  // a corner with data not yet converged (giVisJudged): the level is read without the test
    [loop] for (uint k = 0; k < 8; ++k)
    {
        const int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
        const uint entry = giFind(b, h, giKey(level, nc, c0 + o));
        if (entry == GI_ENTRY_PENDING) continue;
        const uint updates = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES);
        if (updates == 0) continue;  // no information yet
        young = young || !giVisJudged(h, updates);
        const uint4 anchor = b.Load4(h.offAnchor + entry * 16);  // position and packed normal (visibility and radiance)
        const float3 n = giUnpackAnchorNormal(anchor.w);
        const bool seen = giAnchorSeesPointAt(b, h, entry, asfloat(anchor.xyz), n, worldPos, s);  // (else behind a surface from the anchor)
        const float3 wt = lerp(1 - t, t, float3(o));
        const float w = wt.x * wt.y * wt.z;
        const float3 e = w * giIrrMapAt(b, h, entry, n, normal);
        float3 l = 0;
        if (wantRadiance)
        {
            float3 tb, bb;
            giBasis(n, tb, bb);
            const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
            l = w * giTexelRadianceCone(b, h, entry, giHemiOctEncode(local), cone, emitters);
        }
        giKeepRead(b, h, entry, w);
        if (seen)
        {
            sumE += e;
            if (wantRadiance) sumL += l;
            weight += w;
            seenW += w;
        }
        else
        {
            hiddenE += e;
            hiddenL += l;
            hiddenW += w;
        }
    }
    if ((seenW <= 0 || young) && hiddenW > 0)
    {
        sumE += hiddenE;
        if (wantRadiance) sumL += hiddenL;
        weight += hiddenW;
    }
}

// Level search of the lookups: from the point's level (at least minLevel) up to GI_LEVEL_CLIMB coarser levels; when none of
// those has data and minLevel raised the start, down through the finer levels to the point's own level (the cells
// screen probes and nearer rays created there). A ray hit's footprint level is often coarser than every cell that exists
// at its point: stopping at the coarse side returned 0 there (black reflection samples on surfaces the cache covers).
// cone: the radiance's prefilter half-angle (giTexelRadianceCone; 0 = texel resolution, the ray hits' own reads).
template <typename B>
void giCacheLevels(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, bool wantRadiance, uint minLevel, out float3 sumE, out float3 sumL,
                   out float weight, float cone = 0, bool emitters = false)
{
    const uint nc = giNormalClass(normal);
    const uint own = giLevel(h, worldPos), first = max(own, minLevel);
    float3 e = 0, l = 0;
    float w = 0;
    uint level = first;
    [loop] for (uint attempt = 0; attempt < GI_LEVEL_CLIMB && w <= 0 && level <= h.maxLevel; ++attempt, ++level)
        giAccumulateLevel(b, h, worldPos, normal, dir, wantRadiance, nc, level, e, l, w, cone, emitters);
    [loop] for (uint finer = first; w <= 0 && finer > own && first - finer < GI_LEVEL_CLIMB; --finer)
        giAccumulateLevel(b, h, worldPos, normal, dir, wantRadiance, nc, finer - 1, e, l, w, cone, emitters);
    sumE = e;
    sumL = l;
    weight = w;
}

template <typename B>
float3 giCacheIrradianceAt(B b, GiHeader h, float3 worldPos, float3 normal, uint minLevel, out float weight)
{
    float3 sum, unused;
    giCacheLevels(b, h, worldPos, normal, normal, false, minLevel, sum, unused, weight);
    return weight > 0 ? sum / weight : 0;
}

// Screen surfaces (M's per-pixel irradiance, ScreenProbes pad1): the point's level is a step function of its distance to
// the camera, and neighbouring levels hold estimates at different anchors and cell sizes, so a hard level switch drew a
// visible arc (a sphere around the camera) wherever irradiance varies over a cell (D0 play capture 2026-09-26). Over the
// last GI_LEVEL_BAND of a level's range (log2 of the cell size) the result blends linearly into the next coarser level,
// which it equals at the boundary: continuous in distance. The screen probes in the band create and keep that level's
// cells too (GiProbePlace), so the blend has data wherever it applies. Cost: the pixels and probes in the band
// (GI_LEVEL_BAND of the log-distance range) read a second level (8 more cells); their probes keep up to 8 more entries.
#define GI_LEVEL_BAND 0.25
// The blend weight of the next coarser level at a screen point, and the point's own level.
float giLevelBand(GiHeader h, float3 worldPos, out uint own)
{
    const float size = distance(worldPos, h.camera) * h.cellTan;
    own = giLevelForSize(h, size);
    const float u = log2(max(size, 1e-30) / h.cellSize0);  // level own holds u in (own - 1, own]
    return own < h.maxLevel ? saturate((u - ((float)own - GI_LEVEL_BAND)) / GI_LEVEL_BAND) : 0.0;
}
// Screen points (M's per-pixel irradiance): the point's 8 cells of a level, trilinear. Only cells a probe landed in
// exist, and on a surface those are the cells the surface passes through: the stencil's corners across the surface
// (above a floor, behind a wall) have none by construction - on the city at 4K 49 % of the corners looked up [measured,
// GiGate --lookup-stats, 2026-09-26]. A missing corner c therefore takes the value of its partners across the surface:
// c's weight goes to the corner c ^ axis(a) with the share n_a^2 for each axis a whose partner exists (the shares sum to
// 1 over the axes; a floor gives all of it to the corner below or above). An entry there would itself be anchored on the
// surface nearby, so this is the level's own resolution, where a coarser level was blended in before. The shares are
// continuous in the normal, and a corner's existence only changes where its weight is 0 (the stencil's switch), so the
// result is continuous. Weight that finds no partner (cells a probe skipped: grazing views, where a cell spans less than
// the 8 px probe step along the surface) is filled from the next coarser level as before (renormalising or climbing
// only when nothing was found jumped where a missing cell began: one-pixel lines on walls seen edge-on, D0 2026-09-26).
// With the band: E = (1 - beta) E_own + beta E_own+1, E_L = S_L + (1 - W_L) E_L+1 (S_L, W_L: the level's weighted sum
// and weight after the partners), one loop over levels with the share still unassigned, up to GI_FILL_LEVELS levels;
// what remains then is spread over what was found; nothing found: weight 0 (M uses the probes).
// Before the partners every pixel climbed 2.9 levels on average (77 % of pixels more than one) and evaluated 11.9
// entry maps [measured, city 4K].
#define GI_FILL_LEVELS 4u
// Corner c's weight after the partners: 'has' = bit c set when corner c has data; t = the trilinear fraction; share =
// n * n (sums to 1). Computed per corner (no arrays: the caller's loop keeps one map evaluation in the code).
float giScreenCornerWeight(uint has, uint c, float3 t, float3 share)
{
    if ((has & (1u << c)) == 0) return 0;
    const float3 o = float3(c & 1, (c >> 1) & 1, c >> 2);
    const float3 wt = lerp(1 - t, t, o);
    float w = wt.x * wt.y * wt.z;
    [unroll] for (uint a = 0; a < 3; ++a)
    {
        const uint p = c ^ (1u << a);
        if ((has & (1u << p)) != 0) continue;
        const float3 wp = lerp(1 - t, t, float3(p & 1, (p >> 1) & 1, p >> 2));
        w += wp.x * wp.y * wp.z * share[a];
    }
    return w;
}
// (selects, not a dynamic component index: that made local arrays, 4 per level in r.gi.screen and 16 in ShadeOpaque)
uint giScreenPick(uint4 lo, uint4 hi, uint c)
{
    const uint4 v = c < 4 ? lo : hi;
    return (c & 2) ? ((c & 1) ? v.w : v.z) : ((c & 1) ? v.y : v.x);
}
// One level from its resolved corners (entries and packed anchor normals of corners 0..3 and 4..7, 'has' as above; the
// caller resolves them: giScreenCell per pixel, or the group's table, GiCacheTile.hlsli).
template <typename B>
void giScreenLevel(B b, GiHeader h, uint4 entryLo, uint4 entryHi, uint4 anchorLo, uint4 anchorHi, uint has, float3 t, float3 normal, out float3 sum,
                   out float w)
{
    const float3 share = normal * normal;
    sum = 0;
    w = 0;
    [loop] for (uint c = 0; c < 8; ++c)
    {
        const float wc = giScreenCornerWeight(has, c, t, share);
        if (wc <= 0) continue;
        sum += wc * giIrrMapAt(b, h, giScreenPick(entryLo, entryHi, c), giUnpackAnchorNormal(giScreenPick(anchorLo, anchorHi, c)), normal);
        w += wc;
    }
}
// Visibility re-weights among the level's corners: the seen corners' sum is scaled to the weight the corners with data
// ('hasData') have, so a rejected corner hands its weight to the seen ones of its own level. Counted as a missing cell
// instead, a rejected corner's weight went (after its normal-axis partner, usually rejected too) to the next coarser level,
// whose larger cells reach through the wall: the outside's sky light the test exists to keep out came back from there
// (bright bluish blotches on the bathhouse floor along its walls, 20379fb, 2026-09-28).
void giScreenVisRenorm(uint hasData, uint seen, float3 t, float3 normal, inout float3 s, inout float w)
{
    if (seen == hasData || w <= 0) return;
    const float3 share = normal * normal;
    float wData = 0;
    [unroll] for (uint c = 0; c < 8; ++c) wData += giScreenCornerWeight(hasData, c, t, share);
    s *= wData / w;
    w = wData;
}
// The corners of 'has' whose anchor does not see the point cleared (giAnchorSeesPoint): they count as missing cells.
// judged: GI_VIS_JUDGED (every corner with data converged, giVisJudged), GI_VIS_YOUNG (one is not: the level is read without
// the test and nothing more is loaded), GI_VIS_UNKNOWN (the update counts are read here).
#define GI_VIS_YOUNG 0u
#define GI_VIS_JUDGED 1u
#define GI_VIS_UNKNOWN 2u
template <typename B>
uint giScreenSeen(B b, GiHeader h, uint4 entryLo, uint4 entryHi, uint4 anchorLo, uint4 anchorHi, uint has, float3 p, float cellSize, uint judged = GI_VIS_UNKNOWN)
{
    if ((h.flags & 2u) == 0 || judged == GI_VIS_YOUNG || has == 0) return has;
    uint seen = has;
    bool all = true;
    [loop] for (uint c = 0; c < 8; ++c)
    {
        const bool on = (has & (1u << c)) != 0;
        const uint entry = on ? giScreenPick(entryLo, entryHi, c) : 0u;
        if (judged == GI_VIS_UNKNOWN) all = all && (!on || giVisJudged(h, b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES)));
        const bool sees = giAnchorSeesPoint(b, h, entry, giUnpackAnchorNormal(on ? giScreenPick(anchorLo, anchorHi, c) : 0u), p, cellSize);
        if (on && !sees) seen &= ~(1u << c);
    }
    return all && seen != 0 ? seen : has;  // (a level with a young corner is read without the test; visibility never empties a level)
}
// A cell's entry with data (GI_ENTRY_PENDING: none, or not updated yet), its packed anchor normal and update count.
template <typename B>
uint giScreenCellUpdates(B b, GiHeader h, uint64_t key, out uint anchor, out uint updates)
{
    uint entry = giFind(b, h, key);
    anchor = 0;
    updates = 0;
    if (entry != GI_ENTRY_PENDING)
    {
        // Both words depend on the entry only: issued together (one round trip, not the anchor after the count).
        updates = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES);
        const uint packed = b.Load(h.offAnchor + entry * 16 + 12);
        if (updates == 0) entry = GI_ENTRY_PENDING;
        else anchor = packed;
    }
    return entry;
}
template <typename B>
uint giScreenCell(B b, GiHeader h, uint64_t key, out uint anchor)
{
    uint updates;
    return giScreenCellUpdates(b, h, key, anchor, updates);
}
template <typename B>
float3 giCacheIrradianceScreen(B b, GiHeader h, float3 worldPos, float3 normal, out float weight)
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
        uint has = 0, judged = GI_VIS_JUDGED;
        [unroll] for (uint c = 0; c < 8; ++c)
        {
            uint anchor, updates;
            const uint entry = giScreenCellUpdates(b, h, giKey(level, nc, c0 + int3(c & 1, (c >> 1) & 1, c >> 2)), anchor, updates);
            if (c < 4) entryLo[c] = entry, anchorLo[c] = anchor;
            else entryHi[c - 4] = entry, anchorHi[c - 4] = anchor;
            if (entry != GI_ENTRY_PENDING) has |= 1u << c;
            if (entry != GI_ENTRY_PENDING && !giVisJudged(h, updates)) judged = GI_VIS_YOUNG;
        }
        const uint hasData = has;
        has = giScreenSeen(b, h, entryLo, entryHi, anchorLo, anchorHi, has, worldPos, giCellSize(h, level), judged);
        float3 s;
        float w;
        giScreenLevel(b, h, entryLo, entryHi, anchorLo, anchorHi, has, f - floor(f), normal, s, w);
        giScreenVisRenorm(hasData, has, f - floor(f), normal, s, w);
        const float take = k == 0 ? 1 - beta : 1.0;  // the band passes beta of the point on to the next level
        result += (remain * take) * s;
        remain *= 1 - take * w;
    }
    weight = 1 - remain;
    return weight > 0 ? result / weight : 0;
}

float3 giCacheIrradiance(GiSrvs s, float3 worldPos, float3 normal)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[s.cache];
    const GiHeader h = giHeader(b);
    float w;
    return giCacheIrradianceAt(b, h, worldPos, normal, 0, w);
}

// Anchor: float3 position, uint normal (octahedral snorm16 x 2), 16 B.
template <typename B>
float3 giAnchorPosition(B b, GiHeader h, uint entry) { return asfloat(b.Load3(h.offAnchor + entry * 16)); }
template <typename B>
float3 giAnchorNormal(B b, GiHeader h, uint entry) { return giUnpackAnchorNormal(b.Load(h.offAnchor + entry * 16 + 12)); }

// Emitter texels (after the deterministic anchors' table): per entry 64 RGB9E5 words x GI_STORE_SCALE, the radiance of
// the analytic area lights (raytracing.emitters, the stable ones) its texel rays met, apart from the texels. The texels
// hold everything else: ray hits read them with their own next-event sample of the lights (the lights' specular once,
// at its true shape), and the K path (the probes' maps: texels + emitter texels) and the cache readers without a light
// loop (glass, water) read both: M leaves those lights' specular to the reflection paths there (COVERAGE 12.4
// structure 2, ARCHITECTURE 2.13: the interior's -3.47 ms lever), prefiltered by the lobe's cone.
uint giEmitterOffset(GiHeader h) { return h.offSlotAnchor + h.tableSlots * 8; }
template <typename B>
float3 giEmitterTexel(B b, GiHeader h, uint entry, uint2 t) { return giIrrUnpack(b.Load(giEmitterOffset(h) + (entry * GI_TEXEL_COUNT + t.y * GI_TEXELS + t.x) * 4)); }

// Incident radiance from a direction, bilinear over the 8 x 8 texels of one entry; emitters: plus its emitter texels.
// A row's two texels are adjacent words (8 B each): one Load4 per row when both are inside the map, one Load2 when the
// clamp makes them the same texel (the map's edge) - the same texels, weights and summation order as a load per texel,
// half the load instructions (every ray hit's radiance read and every cone tap goes through here).
template <typename B>
float3 giTexelRadiance(B b, GiHeader h, uint entry, float2 uv, bool emitters = false)
{
    const float2 x = uv * GI_TEXELS - 0.5;
    const int2 i0 = int2(floor(x));
    const float2 f = x - floor(x);
    const uint2 lo = uint2(clamp(i0, 0, int(GI_TEXELS) - 1)), hi = uint2(clamp(i0 + 1, 0, int(GI_TEXELS) - 1));
    const bool pair = hi.x == lo.x + 1;
    float3 r = 0;
    [unroll] for (uint row = 0; row < 2; ++row)
    {
        const uint y = row ? hi.y : lo.y;
        const uint address = h.offTexels + (entry * GI_TEXEL_COUNT + y * GI_TEXELS + lo.x) * 8;
        uint4 v;
        if (pair) v = b.Load4(address);
        else
        {
            v.xy = b.Load2(address);
            v.zw = v.xy;
        }
        [unroll] for (uint c = 0; c < 2; ++c)
        {
            const uint2 w2 = c ? v.zw : v.xy;
            const float w = (c ? f.x : 1 - f.x) * (row ? f.y : 1 - f.y);
            float3 texel = float3(f16tof32(w2.x), f16tof32(w2.x >> 16), f16tof32(w2.y));
            if (emitters) texel += giEmitterTexel(b, h, entry, uint2(c ? hi.x : lo.x, y));
            r += w * texel;
        }
    }
    return r * GI_LOAD_SCALE;
}

// Incident radiance from a direction prefiltered by a cone of half-angle 'cone' (radians), one entry. The level of detail
// is the K path's (ScreenProbes.hlsli giProbeFootprintRadiance: the map mip whose texel cone matches the cone with the
// Nyquist margin 1.5, 8 x 8 texel ~10.1 deg half-angle), so the planar views' and the main view's K values filter alike:
// lod = log2(cone / (1.5 x 0.1763)) in [0, 2]. Mip lod of the 8 x 8 map read bilinearly is a box of 2^lod texels; here it
// is formed from the texels themselves (no stored mips): four bilinear taps at (+-o, +-o) texels around the direction,
// o = (2^lod - 1) / 2, the mean of four 2 x 2 texel blocks at integer lods (lod 1: the 3 x 3 tent, lod 2: the 4 x 4 box)
// and continuous between. Cost: 16 texel loads instead of 4 where lod > 0. Below the lod 0 cone (mirror-like lobes) the
// bilinear texel value as before.
template <typename B>
float3 giTexelRadianceCone(B b, GiHeader h, uint entry, float2 uv, float cone, bool emitters = false)
{
    const float lod = clamp(log2(max(cone, 1e-3) / (1.5 * 0.1763)), 0.0, 2.0);
    if (lod <= 0) return giTexelRadiance(b, h, entry, uv, emitters);
    const float o = (exp2(lod) - 1) * 0.5 / GI_TEXELS;
    float3 r = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
        r += giTexelRadiance(b, h, entry, uv + float2(k & 1 ? o : -o, k & 2 ? o : -o), emitters);
    return r * 0.25;
}

// Irradiance and incident radiance from direction dir at a surface point, in one pass over the cells (ray hits need
// both): trilinear over the 8 cells of the point's level (at least minLevel) and normal class like giCacheIrradianceAt;
// each existing updated entry contributes its SH irradiance and its texel-resolution radiance toward dir (bilinear over
// its 8 x 8 hemisphere, in its own frame); level search as giCacheLevels; zeros when no level has one.
template <typename B>
void giCacheLightingAt(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, uint minLevel, out float3 irradiance, out float3 radiance)
{
    float3 sumE, sumL;
    float weight;
    giCacheLevels(b, h, worldPos, normal, dir, true, minLevel, sumE, sumL, weight);
    irradiance = weight > 0 ? sumE / weight : 0;
    radiance = weight > 0 ? sumL / weight : 0;
}

// Incident radiance from direction dir at a surface point with normal 'normal' (v1.6 request 20260925_R_hit_shading.md):
// the surface's own entries (its normal class), trilinear over 8 cells with level climbing, prefiltered by the lobe's
// cone coneHalfAngle (giTexelRadianceCone: the K path's level of detail; texel resolution for lobes within a texel).
// Readers: M's planar views, glass (TranslucentComposite), W's water fallback. Without the prefilter a rough lobe read one
// texel's value: a sharp, texel-shaped image of bright content (sky openings, lit windows) on rough surfaces.
// emitters: add the emitter texels (readers that shade no area light's specular themselves: glass, water); M's planar
// views shade every light by LTC and read the texels alone.
float3 giCacheRadiance(GiSrvs s, float3 worldPos, float3 normal, float3 dir, float coneHalfAngle, bool emitters = false)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[s.cache];
    const GiHeader h = giHeader(b);
    float3 sumE, sumL;
    float weight;
    giCacheLevels(b, h, worldPos, normal, dir, true, 0, sumE, sumL, weight, coneHalfAngle, emitters);
    return weight > 0 ? sumL / weight : 0;
}

// Deprecated (v1): keys the entry by dir's normal class instead of the surface's, so it can read another surface's
// entry or none; use the overload with the surface normal above.
// Incident radiance from direction 'dir' at worldPos. v1 returns the texel-resolution radiance of the nearest-cell entry
// of dir's normal class; the lobe prefilter by coneHalfAngle (ARCHITECTURE 2.6 K path) comes with the reflection work.
float3 giCacheRadiance(GiSrvs s, float3 worldPos, float3 dir, float coneHalfAngle)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[s.cache];
    const GiHeader h = giHeader(b);
    const uint level = giLevel(h, worldPos);
    const int3 cell = int3(floor(worldPos / giCellSize(h, level)));
    const uint entry = giFind(b, h, giKey(level, giNormalClass(dir), cell));
    if (entry == GI_ENTRY_PENDING) return 0;
    const float3 n = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(n, t, bt);
    const float3 local = float3(dot(dir, t), dot(dir, bt), max(dot(dir, n), 0.0));
    return giTexelRadiance(b, h, entry, giHemiOctEncode(local));
}

#endif
