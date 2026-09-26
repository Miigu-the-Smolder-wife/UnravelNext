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
#define GI_SH_HISTORY 68u      // updates since the last reset (Jacobi phase, then averaging)
#define GI_SH_EPOCH 72u        // lighting epoch of that history

// Entry irradiance map (design 2.5 revision, request 18): E(n) at the 9 x 9 hemispherical octahedral directions around
// the entry's anchor normal (texel (i, j) = giHemiOctDecode(((i, j) + 0.5) / 9); the pole, the anchor normal itself, is
// the centre texel), estimated per ray: every update's 64 rays add L max(0, n_j . w) dw, dw = the ray's solid-angle
// weight (2 / |p|^3 x the texel's UV area for the hemispherical octahedral map). No texel averaging (a thin band of
// horizon light is not smeared over a texel) and no SH truncation. RGB9E5 x GI_STORE_SCALE, 81 words + 3 padding.
// Evaluated with Catmull-Rom (16 texels), clamped at 0.
#define GI_IRR_N 9u
#define GI_IRR_STRIDE 336u

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
    uint hitCount0, hitCount1, jacobiUpdates, historyMax;
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
    h.hitCount0 = r7.x; h.hitCount1 = r7.y; h.jacobiUpdates = r7.z; h.historyMax = r7.w;
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

// Irradiance (x 1, not stored scale) of an entry for normal n from its map; normals below the entry's hemisphere use
// its horizon.
template <typename B>
float3 giIrrMap(B b, GiHeader h, uint entry, float3 n)
{
    const float3 na = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(na, t, bt);
    float3 local = float3(dot(n, t), dot(n, bt), max(dot(n, na), 0.0));
    local = dot(local, local) > 1e-12 ? normalize(local) : float3(0, 0, 1);
    const float2 e = giHemiOctEncode(local) * (float)GI_IRR_N - 0.5;
    const int2 i0 = int2(floor(e));
    const float2 f = e - float2(i0);
    const float4 wx = giCatmullRom(f.x), wy = giCatmullRom(f.y);
    const uint base = h.offIrr + entry * GI_IRR_STRIDE;
    float3 sum = 0;
    [unroll] for (uint jy = 0; jy < 4; ++jy)
    {
        const uint iy = (uint)clamp(i0.y - 1 + (int)jy, 0, (int)GI_IRR_N - 1);
        float3 row = 0;
        [unroll] for (uint jx = 0; jx < 4; ++jx)
        {
            const uint ix = (uint)clamp(i0.x - 1 + (int)jx, 0, (int)GI_IRR_N - 1);
            row += wx[jx] * giIrrUnpack(b.Load(base + (iy * GI_IRR_N + ix) * 4));
        }
        sum += wy[jy] * row;
    }
    return max(sum, 0.0) * GI_LOAD_SCALE;
}

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
// be left at its first, unconverged update.
void giKeepRead(ByteAddressBuffer b, GiHeader h, uint entry) {}
void giKeepRead(RWByteAddressBuffer b, GiHeader h, uint entry);

// One level's trilinear accumulation over the 8 cells of the point (entries that exist and have been updated).
template <typename B>
void giAccumulateLevel(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, bool wantRadiance, uint nc, uint level, inout float3 sumE,
                       inout float3 sumL, inout float weight)
{
    const float s = giCellSize(h, level);
    const float3 f = worldPos / s - 0.5;
    const int3 c0 = int3(floor(f));
    const float3 t = f - floor(f);
    [loop] for (uint k = 0; k < 8; ++k)
    {
        const int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
        const uint entry = giFind(b, h, giKey(level, nc, c0 + o));
        if (entry == GI_ENTRY_PENDING || b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) == 0) continue;  // no information yet
        const float3 wt = lerp(1 - t, t, float3(o));
        const float w = wt.x * wt.y * wt.z;
        float sv;
        sumE += w * giShIrradiance(b, h, entry, normal, sv);
        if (wantRadiance)
        {
            const float3 n = giAnchorNormal(b, h, entry);
            float3 tb, bb;
            giBasis(n, tb, bb);
            const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
            sumL += w * giTexelRadiance(b, h, entry, giHemiOctEncode(local));
        }
        weight += w;
        giKeepRead(b, h, entry);
    }
}

// Level search of the lookups: from the point's level (at least minLevel) up to GI_LEVEL_CLIMB coarser levels; when none of
// those has data and minLevel raised the start, down through the finer levels to the point's own level (the cells
// screen probes and nearer rays created there). A ray hit's footprint level is often coarser than every cell that exists
// at its point: stopping at the coarse side returned 0 there (black reflection samples on surfaces the cache covers).
template <typename B>
void giCacheLevels(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, bool wantRadiance, uint minLevel, out float3 sumE, out float3 sumL,
                   out float weight)
{
    const uint nc = giNormalClass(normal);
    const uint own = giLevel(h, worldPos), first = max(own, minLevel);
    float3 e = 0, l = 0;
    float w = 0;
    uint level = first;
    [loop] for (uint attempt = 0; attempt < GI_LEVEL_CLIMB && w <= 0 && level <= h.maxLevel; ++attempt, ++level)
        giAccumulateLevel(b, h, worldPos, normal, dir, wantRadiance, nc, level, e, l, w);
    [loop] for (uint finer = first; w <= 0 && finer > own && first - finer < GI_LEVEL_CLIMB; --finer)
        giAccumulateLevel(b, h, worldPos, normal, dir, wantRadiance, nc, finer - 1, e, l, w);
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
float3 giAnchorNormal(B b, GiHeader h, uint entry)
{
    const uint packed = b.Load(h.offAnchor + entry * 16 + 12);
    const float2 e = float2(int2(packed << 16, packed) >> 16) / 32767.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

// Incident radiance from a direction, bilinear over the 8 x 8 texels of one entry.
template <typename B>
float3 giTexelRadiance(B b, GiHeader h, uint entry, float2 uv)
{
    const float2 x = uv * GI_TEXELS - 0.5;
    const int2 i0 = int2(floor(x));
    const float2 f = x - floor(x);
    float3 r = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const uint2 t = uint2(clamp(i0 + o, 0, int(GI_TEXELS) - 1));
        const uint2 v = b.Load2(h.offTexels + (entry * GI_TEXEL_COUNT + t.y * GI_TEXELS + t.x) * 8);
        const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        r += w * float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y));
    }
    return r * GI_LOAD_SCALE;
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
// the surface's own entries (its normal class), trilinear over 8 cells with level climbing (giCacheLightingAt), texel
// resolution (8 x 8 hemisphere, ~22 deg texels; coneHalfAngle reserved for the lobe prefilter).
float3 giCacheRadiance(GiSrvs s, float3 worldPos, float3 normal, float3 dir, float coneHalfAngle)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[s.cache];
    const GiHeader h = giHeader(b);
    float3 irradiance, radiance;
    giCacheLightingAt(b, h, worldPos, normal, dir, 0, irradiance, radiance);
    return radiance;
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
