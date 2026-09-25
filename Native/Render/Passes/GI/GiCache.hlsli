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

template <typename B>
float3 giShIrradiance(B b, GiHeader h, uint entry, float3 n, out float sunVisibility)
{
    const uint a = h.offSh + entry * GI_SH_STRIDE;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32), w3 = b.Load4(a + 48);
    const uint w[14] = { w0.x, w0.y, w0.z, w0.w, w1.x, w1.y, w1.z, w1.w, w2.x, w2.y, w2.z, w2.w, w3.x, w3.y };
    float y[9];
    giShBasis(n, y);
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const uint i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        const float r = f16tof32(w[i0 >> 1] >> ((i0 & 1) * 16));
        const float g = f16tof32(w[i1 >> 1] >> ((i1 & 1) * 16));
        const float bl = f16tof32(w[i2 >> 1] >> ((i2 & 1) * 16));
        e += float3(r, g, bl) * y[k];
    }
    sunVisibility = f16tof32(w[13] >> 16);
    return max(e * GI_LOAD_SCALE, 0.0);
}

// Irradiance (indirect + sky, no direct sun) at a surface point: trilinear over the 8 cells of the point's level (at least
// minLevel) and normal class, entries that exist and have been updated at least once (renormalised); coarser levels when
// none does (up to GI_LEVEL_CLIMB more: points no probe sees only have the coarse cells GI rays created there).
#define GI_LEVEL_CLIMB 6u

// Read hook of the lookups below, per contributing entry. Read-only readers (M's shading) do nothing; R's ray hits read
// through the RW cache and keep what they read alive and requested (GiInternal.hlsli): an entry only readers see must not
// be left at its first, unconverged update.
void giKeepRead(ByteAddressBuffer b, GiHeader h, uint entry) {}
void giKeepRead(RWByteAddressBuffer b, GiHeader h, uint entry);

template <typename B>
float3 giCacheIrradianceAt(B b, GiHeader h, float3 worldPos, float3 normal, uint minLevel, out float weight)
{
    const uint nc = giNormalClass(normal);
    uint level = max(giLevel(h, worldPos), minLevel);
    float3 sum = 0;
    weight = 0;
    [loop] for (uint attempt = 0; attempt < GI_LEVEL_CLIMB && weight <= 0 && level <= h.maxLevel; ++attempt, ++level)
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
            sum += w * giShIrradiance(b, h, entry, normal, sv);
            weight += w;
            giKeepRead(b, h, entry);
        }
    }
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
// its 8 x 8 hemisphere, in its own frame); coarser levels when none exists; zeros when no level has one.
template <typename B>
void giCacheLightingAt(B b, GiHeader h, float3 worldPos, float3 normal, float3 dir, uint minLevel, out float3 irradiance, out float3 radiance)
{
    const uint nc = giNormalClass(normal);
    uint level = max(giLevel(h, worldPos), minLevel);
    float3 sumE = 0, sumL = 0;
    float weight = 0;
    [loop] for (uint attempt = 0; attempt < GI_LEVEL_CLIMB && weight <= 0 && level <= h.maxLevel; ++attempt, ++level)
    {
        const float s = giCellSize(h, level);
        const float3 f = worldPos / s - 0.5;
        const int3 c0 = int3(floor(f));
        const float3 t = f - floor(f);
        [loop] for (uint k = 0; k < 8; ++k)
        {
            const int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
            const uint entry = giFind(b, h, giKey(level, nc, c0 + o));
            if (entry == GI_ENTRY_PENDING || b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) == 0) continue;
            const float3 wt = lerp(1 - t, t, float3(o));
            const float w = wt.x * wt.y * wt.z;
            float sv;
            sumE += w * giShIrradiance(b, h, entry, normal, sv);
            const float3 n = giAnchorNormal(b, h, entry);
            float3 tb, bb;
            giBasis(n, tb, bb);
            const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
            sumL += w * giTexelRadiance(b, h, entry, giHemiOctEncode(local));
            weight += w;
            giKeepRead(b, h, entry);
        }
    }
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
