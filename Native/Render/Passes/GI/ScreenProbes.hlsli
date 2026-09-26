// Screen probes (R track, ARCHITECTURE 2.5, 2.6; INTERFACES 5.6 public API: ProbeSrvs, screenProbeIrradiance,
// screenProbeRadiance). Consumer: M's shading kernel (main view). One probe per gi.screen_probe_spacing_px square tile,
// at the tile corners (design revision 12.3): probe (i, j) at the pixel corner (8i, 8j), (ceil(W / 8) + 1) x
// (ceil(H / 8) + 1) probes, so the pixels of tile (tx, ty) interpolate probes tx..tx+1, ty..ty+1 only. Filled every
// frame from the world radiance cache (no rays):
//   - irradiance: trilinear L2 SH of the cache (indirect + sky, no direct sun) at the probe's surface point;
//   - near occlusion of that point (depth-buffer taps within the sub-cell radius, at probe resolution);
//   - radiance: the incident radiance of the probe's cache entry as an 8 x 8 hemispherical octahedral map around that
//     entry's normal, with 4 x 4 and 2 x 2 mips (solid-angle weighted) for the K reflection path's cone prefilter.
//
// Use: ProbeSrvs{ c.srv(view.screenProbes), c.srv(view.screenProbes), 0, 0 } (both fields: the one texture; the pass
// declares view.screenProbes SrvCompute) with the main view's frame constants bound (b1).
//
// Texture: RGBA32_UINT, (probesX * 8) x (probesY * 5 + 1).
//   Records in five planes of probesX x probesY texels (rows 4 probesY .. 5 probesY - 1, plane k at columns
//   k probesX ..), so a pixel's four neighbouring probes are neighbouring texels:
//     plane 0: { world position (fp32 x 3), octahedral normal, 0 = no surface } (the footprint's only read)
//     planes 1-4: 27 fp16 irradiance SH coefficients x GI_STORE_SCALE, then unorm16 occlusion (words 0-13)
//     plane 5: { the radiance map's frame normal (octahedral), map block: probe x | y << 16 }
//   Radiance maps, probe (i, j) = the block of texels (8i .. 8i+7, 4j .. 4j+3):
//     row 0: texel 4 = the 2 x 2 mip (RGB9E5 x 4)
//     rows 1-2: the 8 x 8 map, 4 RGB9E5 texels per RGBA32 texel, row-major
//     row 3: texels 0-3 = the 4 x 4 mip, texel 5 = the cache entry the map comes from (GiProbeGather -> GiProbeMaps)
//   The map is written once per cache entry, in the block of the lowest probe reading that entry; other probes' map
//   texels are stale and are reached through plane 5's block.
// Last row (5 probesY), texel 0 = { spacing px, probesX, probesY, 0 }.
//
// view.screenProbeMaps (v1.13): the same K-path maps as a hardware-filtered atlas, (probesX * 14) x (probesY * 8),
// R32_UINT written / R9G9B9E5_SHAREDEXP sampled, values x GI_STORE_SCALE. Map block (i, j) has its level L tile
// (n = 8 >> L texels square) at x0_L + i n, j n, x0 = 0, 8 probesX, 12 probesX; only owner blocks are written.
// screenProbeGather reads it (ProbeSrvs.pad0 = its SRV); a tile is sampled with coordinates clamped to its texel centres,
// which equals clamp-to-edge bilinear and never reads a neighbouring tile. Radiance values are x GI_STORE_SCALE (1/64).
//
// Tile cache (request 20260925_M_probe_tile_cache, design revision 1 4.4): a kernel whose groups are 8 x 8 pixel tiles
// defines GI_PROBE_TILE_CACHE before including this file, calls giProbeTileLoad (lanes 0..54) and a group barrier, then
// screenProbeGatherTile. With gi.screen_probe_spacing_px = 8 (GiSystem enforces it) a tile's pixels read exactly the 3 x 3
// probes around it, so their records and the header come from groupshared; only the atlas taps read memory. The math
// is the same code (templates over the record source), so the result equals screenProbeGather bit for bit; K radiance
// needs the atlas (ProbeSrvs.pad0).
#ifndef UNX_GI_SCREENPROBES_HLSLI
#define UNX_GI_SCREENPROBES_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/GI/GiCache.hlsli"

// pad0 = the K-path maps atlas SRV (v1.13). pad1 = the GI cache SRV + 1 (0 = none): with it the irradiance comes from the
// cache's per-ray 9 x 9 irradiance maps at the pixel's own position and normal (design 2.5 revision D, step 1: exact at
// the entries' grid directions, Catmull-Rom between; FrameResources::giCache declared SrvCompute by the caller); without
// it from the probes' SH (interim, L2 truncation).
struct ProbeSrvs
{
    uint probes, occlusion, pad0, pad1;
};

struct GiProbeRecord
{
    float3 sh[9];
    float occlusion;
};


float3 giProbeOctDecode(uint packed)
{
    const float2 e = float2(int2(packed << 16, packed) >> 16) / 32767.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

float3 giUnpackRgb9e5(uint v)
{
    const float scale = asfloat(((v >> 27) + 103u) << 23);  // 2^(e - 24) exactly (bias 15, 9-bit mantissa): no exp2
    return float3(v & 0x1FFu, (v >> 9) & 0x1FFu, (v >> 18) & 0x1FFu) * scale;
}

// Record sources: the probes texture, or (GI_PROBE_TILE_CACHE) a tile's groupshared copy of its 2 x 2 corner probes.
uint4 giProbePlane(Texture2D<uint4> t, uint2 probe, uint plane, int2 count) { return t.Load(int3(probe.x + plane * count.x, count.y * 4 + probe.y, 0)); }
uint4 giProbeHeader(Texture2D<uint4> t)
{
    uint width, height;
    t.GetDimensions(width, height);
    return t.Load(int3(0, height - 1, 0));
}

#ifdef GI_PROBE_TILE_CACHE
groupshared uint4 gs_giProbe[4][6];  // planes 0..5 of the corner probes tile .. tile + 1, row-major
groupshared uint4 gs_giHeader;       // { spacing, probesX, probesY, 0 }

struct GiProbeTile
{
    int2 first;  // the tile: slot of a probe p = p - first (0 or 1 for the probes a tile's pixels read)
};

uint4 giProbePlane(GiProbeTile t, uint2 probe, uint plane, int2 count)
{
    const int2 slot = int2(probe) - t.first;
    return gs_giProbe[slot.y * 2 + slot.x][plane];
}

// Probe counts of the main view (b1): ceil(view / 8) + 1 each way (corner probes, spacing 8 enforced by GiSystem).
int2 giProbeCountOfView() { return int2((uint2(g_viewWidth, g_viewHeight) + 7) / 8 + 1); }
uint4 giProbeHeader(GiProbeTile t) { return gs_giHeader; }

// Split form of giProbeTileLoad for kernels that issue their own reads in between (M's shading kernel: the pixel's
// word, G-buffer and depth and S's overflow head go out together with the tile's records; the records are stored once
// those are in flight). The probe counts are the main view's (giProbeCountOfView: frame constants), so no header read
// has to come first; lane 24 returns the header they define. 'count' is ignored (kept for the callers of the 3 x 3
// form; it was ceil(view / 8), which the corner grid no longer uses).
// [M measurement, city 4K: shading 1.257 -> 1.191 ms, 3 alternating rounds, together with M's reordered kernel.]
uint4 giProbeTileFetch(ProbeSrvs s, uint2 tile, uint lane, int2 count)
{
    count = giProbeCountOfView();
    if (lane > 24) return 0;
    if (lane == 24) return uint4(8, count.x, count.y, 0);
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    const uint k = lane / 6, plane = lane % 6;
    const int2 probe = min(int2(tile) + int2(k & 1, k >> 1), count - 1);
    return giProbePlane(t, uint2(probe), plane, count);
}
void giProbeTileStore(uint lane, uint4 value)
{
    if (lane < 24) gs_giProbe[lane / 6][lane % 6] = value;
    else if (lane == 24) gs_giHeader = value;
}

// Lanes 0..23 load (probe k = lane / 6 at tile + (k & 1, k >> 1), plane lane % 6), lane 24 the header; the caller then
// syncs the group.
void giProbeTileLoad(ProbeSrvs s, uint2 tile, uint lane)
{
    if (lane > 24) return;
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    const uint4 header = giProbeHeader(t);
    if (lane == 24)
    {
        gs_giHeader = header;
        return;
    }
    const int2 count = int2(header.y, header.z);
    const uint k = lane / 6, plane = lane % 6;
    const int2 probe = min(int2(tile) + int2(k & 1, k >> 1), count - 1);
    gs_giProbe[k][plane] = giProbePlane(t, uint2(probe), plane, count);
}
#endif

// The footprint's part of a probe record: world position and normal; false when the probe has no surface.
template <typename Src>
bool giLoadProbeSurface(Src t, uint2 probe, int2 count, out float3 position, out float3 normal)
{
    const uint4 a = giProbePlane(t, probe, 0, count);
    position = asfloat(a.xyz);
    normal = giProbeOctDecode(a.w);
    return a.w != 0;
}

template <typename Src>
GiProbeRecord giLoadProbeRecord(Src t, uint2 probe, int2 count)
{
    const uint4 a = giProbePlane(t, probe, 1, count), b = giProbePlane(t, probe, 2, count), c = giProbePlane(t, probe, 3, count),
                d = giProbePlane(t, probe, 4, count);
    const uint w[14] = { a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, d.x, d.y };
    GiProbeRecord r;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const uint i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        r.sh[k] = float3(f16tof32(w[i0 >> 1] >> ((i0 & 1) * 16)), f16tof32(w[i1 >> 1] >> ((i1 & 1) * 16)), f16tof32(w[i2 >> 1] >> ((i2 & 1) * 16))) * 64.0;  // GI_LOAD_SCALE
    }
    r.occlusion = (w[13] >> 16) / 65535.0;
    return r;
}

float3 giEvalShIrradiance(float3 sh[9], float3 n)
{
    const float y[9] = { 0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
                         0.315392 * (3 * n.z * n.z - 1), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y) };
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k) e += sh[k] * y[k];
    return max(e, 0.0);
}

// Bilinear radiance of a probe's map at mip 'level' for hemispherical octahedral coordinates uv (clamp to edge),
// x GI_STORE_SCALE. Each packed RGBA32 texel is loaded once: mip 2 (2 x 2) is one texel, a mip 1 row (4 texels) is one
// texel, a mip 0 row (8 texels) two; the taps pick their components.
float3 giProbeMapBilinear(Texture2D<uint4> t, uint2 probe, uint level, float2 uv)
{
    const uint x = probe.x * 8, y = probe.y * 4;
    const uint n = 8u >> level;
    const float2 f0 = uv * n - 0.5;
    const int2 i0 = int2(floor(f0));
    const float2 f = f0 - floor(f0);
    const uint2 a = uint2(clamp(i0, 0, int(n) - 1)), b = uint2(clamp(i0 + 1, 0, int(n) - 1));
    uint p00, p10, p01, p11;
    if (level == 2)
    {
        const uint4 v = t.Load(int3(x + 4, y, 0));  // texel index ty * 2 + tx
        p00 = v[a.y * 2 + a.x];
        p10 = v[a.y * 2 + b.x];
        p01 = v[b.y * 2 + a.x];
        p11 = v[b.y * 2 + b.x];
    }
    else if (level == 1)
    {
        const uint4 r0 = t.Load(int3(x + a.y, y + 3, 0)), r1 = t.Load(int3(x + b.y, y + 3, 0));  // row ty, component tx
        p00 = r0[a.x];
        p10 = r0[b.x];
        p01 = r1[a.x];
        p11 = r1[b.x];
    }
    else
    {
        // Row ty is the two packed texels (x + 2 ty % 8 + g, y + 1 + ty / 4), g = tx / 4, component tx % 4.
        const uint4 r0a = t.Load(int3(x + (a.y * 2) % 8 + (a.x >> 2), y + 1 + (a.y >> 2), 0));
        const uint4 r0b = t.Load(int3(x + (a.y * 2) % 8 + (b.x >> 2), y + 1 + (a.y >> 2), 0));
        const uint4 r1a = t.Load(int3(x + (b.y * 2) % 8 + (a.x >> 2), y + 1 + (b.y >> 2), 0));
        const uint4 r1b = t.Load(int3(x + (b.y * 2) % 8 + (b.x >> 2), y + 1 + (b.y >> 2), 0));
        p00 = r0a[a.x & 3];
        p10 = r0b[b.x & 3];
        p01 = r1a[a.x & 3];
        p11 = r1b[b.x & 3];
    }
    return lerp(lerp(giUnpackRgb9e5(p00), giUnpackRgb9e5(p10), f.x), lerp(giUnpackRgb9e5(p01), giUnpackRgb9e5(p11), f.x), f.y);
}

// Four surrounding probes and their weights for a pixel (plane distance and normal agreement); false when none agrees
// (the pixel's own tile probe is then used with weight 1).
struct GiProbeFootprint
{
    int2 probe[4];
    float weight[4];
};

// The footprint for a pixel whose world position the caller already has (M's shading).
template <typename Src>
GiProbeFootprint giProbeFootprintAt(Src t, uint2 pixel, float3 pixelWorld, float3 normal, float linearDepth, out float spacing, out int2 count)
{
    const uint4 header = giProbeHeader(t);
    const float probeSpacing = (float)header.x;
    const int2 probeCount = int2(header.y, header.z);
    spacing = probeSpacing;
    count = probeCount;
    const float2 f = (float2(pixel) + 0.5) / probeSpacing;  // probe i at the pixel corner spacing i
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    GiProbeFootprint fp;
    float total = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 p = clamp(i0 + o, int2(0, 0), probeCount - 1);
        fp.probe[k] = p;
        fp.weight[k] = 0;
        float3 probeWorld, probeNormal;
        if (!giLoadProbeSurface(t, uint2(p), probeCount, probeWorld, probeNormal)) continue;
        const float plane = saturate(1 - abs(dot(normal, probeWorld - pixelWorld)) / max(linearDepth, 1e-6) / 0.02);
        const float agree = saturate(dot(normal, probeNormal));
        const float agree2 = agree * agree;
        fp.weight[k] = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * (plane * plane) * (agree2 * agree2);
        total += fp.weight[k];
    }
    if (total < 1e-4)
    {
        const int2 own = clamp(int2(floor(f + 0.5)), int2(0, 0), probeCount - 1);  // the nearest corner
        fp.probe[0] = own;
        float3 ownWorld, ownNormal;
        fp.weight[0] = giLoadProbeSurface(t, uint2(own), probeCount, ownWorld, ownNormal) ? 1 : 0;
        fp.weight[1] = fp.weight[2] = fp.weight[3] = 0;
        total = fp.weight[0];
    }
    [unroll] for (uint j = 0; j < 4; ++j) fp.weight[j] = total > 0 ? fp.weight[j] / total : 0;
    return fp;
}

GiProbeFootprint giProbeFootprint(Texture2D<uint4> t, uint2 pixel, float3 normal, float linearDepth, out float spacing, out int2 count)
{
    return giProbeFootprintAt(t, pixel, worldFromDepth(float2(pixel), g_nearPlane / max(linearDepth, 1e-6)), normal, linearDepth, spacing, count);
}

// rgb = indirect + sky irradiance (nits * sr) on a surface with this normal; a = near occlusion (1 = open).
float4 screenProbeIrradiance(ProbeSrvs s, uint2 pixel, float3 normal, float linearDepth)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    float spacing;
    int2 count;
    const GiProbeFootprint fp = giProbeFootprint(t, pixel, normal, linearDepth, spacing, count);
    float4 sum = float4(0, 0, 0, 0);
    bool any = false;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        if (fp.weight[k] <= 0) continue;
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(fp.probe[k]), count);
        sum += fp.weight[k] * float4(giEvalShIrradiance(r.sh, normal), r.occlusion);
        any = true;
    }
    return any ? sum : float4(0, 0, 0, 1);
}

// Bilinear radiance of a map block at level L from the atlas (hardware filtering, RGB9E5), x GI_STORE_SCALE.
float3 giProbeAtlasBilinear(Texture2D<float4> atlas, int2 count, uint2 block, uint level, float2 uv)
{
    const float n = (float)(8u >> level);
    const float x0 = level == 0 ? 0.0 : (level == 1 ? 8.0 * count.x : 12.0 * count.x);
    uint w, h;
    atlas.GetDimensions(w, h);
    precise const float2 coordinate = (float2(x0 + block.x * n, block.y * n) + clamp(uv * n, 0.5, n - 0.5)) / float2(w, h);  // see giProbeFootprintRadiance
    return atlas.SampleLevel(g_linearClamp, coordinate, 0).rgb;
}

// The K path over a footprint. Probes that read the same cache entry share one map block (GiProbeMapOwners): their
// weights are merged and each distinct block is looked up once. The second mip is read only when it differs from the
// first (fractional level below the top mip).
// In-block maps (no atlas): only from the probes texture.
float3 giProbeMapBilinearFrom(Texture2D<uint4> t, uint2 probe, uint level, float2 uv) { return giProbeMapBilinear(t, probe, level, uv); }
#ifdef GI_PROBE_TILE_CACHE
float3 giProbeMapBilinearFrom(GiProbeTile t, uint2 probe, uint level, float2 uv) { return 0; }  // the tile path needs the atlas
#endif

template <typename Src>
float3 giProbeFootprintRadiance(Src t, GiProbeFootprint fp, int2 count, float3 dir, float coneHalfAngle, uint atlasSrv = UNX_NONE)
{
    const float lod = clamp(log2(max(coneHalfAngle, 1e-3) / (1.5 * 0.1763)), 0.0, 2.0);  // 0.1763 rad = 10.1 deg
    const uint l0 = (uint)floor(lod), l1 = min(l0 + 1, 2u);
    const float fl = lod - l0;
    uint2 frame[4];
    [unroll] for (uint k = 0; k < 4; ++k)
        frame[k] = fp.weight[k] > 0 ? giProbePlane(t, uint2(fp.probe[k]), 5, count).xy : uint2(0, 0xFFFFFFFFu);
    if (atlasSrv != UNX_NONE)
    {
        // Atlas: every coordinate first, then every sample (in flight together: one memory round trip per pixel instead
        // of one per distinct block and mip), then the same sum in the same order over the same distinct blocks. The
        // coordinates are giProbeAtlasBilinear's ('precise', see below), so the result is unchanged bit for bit. Probes
        // without weight sample a clamped coordinate whose value is not used. [M measurement, city 4K: shading 1.330 ->
        // 1.259 ms, 3 alternating rounds.] Verified with this path in the tile variant only against the per-block path
        // in screenProbeGather (GiAnalytic ProbeTileCompare): 0 of 663,552,000 evaluations differ.
        Texture2D<float4> atlas = ResourceDescriptorHeap[atlasSrv];
        uint aw, ah;
        atlas.GetDimensions(aw, ah);
        const float nA = (float)(8u >> l0), nB = (float)(8u >> l1);
        const float xA = l0 == 0 ? 0.0 : (l0 == 1 ? 8.0 * count.x : 12.0 * count.x);
        const float xB = l1 == 0 ? 0.0 : (l1 == 1 ? 8.0 * count.x : 12.0 * count.x);
        float wk[4];
        float2 cA[4], cB[4];
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            bool seen = false;
            [unroll] for (uint j = 0; j < k; ++j) seen = seen || (fp.weight[j] > 0 && frame[j].y == frame[k].y);
            float w = (fp.weight[k] > 0 && !seen) ? fp.weight[k] : 0;
            [unroll] for (uint j = k + 1; j < 4; ++j)
                if (w > 0 && fp.weight[j] > 0 && frame[j].y == frame[k].y) w += fp.weight[j];
            wk[k] = w;
            precise const float3 n = giProbeOctDecode(frame[k].x);
            const uint2 block = uint2(frame[k].y & 0xFFFFu, frame[k].y >> 16);
            const float sgn = n.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017 basis (GiCache.hlsli giBasis)
            precise const float a = -1.0 / (sgn + n.z);
            precise const float c = n.x * n.y * a;
            precise const float3 tb = float3(1 + sgn * n.x * n.x * a, sgn * c, -sgn * n.x);
            precise const float3 bb = float3(c, sgn + n.y * n.y * a, -n.y);
            precise const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
            precise const float3 v = local / (abs(local.x) + abs(local.y) + local.z);
            precise const float2 uv = float2(v.x + v.y, v.x - v.y) * 0.5 + 0.5;
            precise const float2 ca = (float2(xA + block.x * nA, block.y * nA) + clamp(uv * nA, 0.5, nA - 0.5)) / float2(aw, ah);
            precise const float2 cb = (float2(xB + block.x * nB, block.y * nB) + clamp(uv * nB, 0.5, nB - 0.5)) / float2(aw, ah);
            cA[k] = ca;
            cB[k] = cb;
        }
        float3 rA[4], rB[4];
        [unroll] for (uint k2 = 0; k2 < 4; ++k2) rA[k2] = atlas.SampleLevel(g_linearClamp, cA[k2], 0).rgb;
        const bool two = fl > 0 && l1 != l0;
        [unroll] for (uint k3 = 0; k3 < 4; ++k3) rB[k3] = two ? atlas.SampleLevel(g_linearClamp, cB[k3], 0).rgb : rA[k3];
        float3 total = 0;
        [unroll] for (uint k4 = 0; k4 < 4; ++k4)
        {
            float3 r = rA[k4];
            if (two) r = lerp(r, rB[k4], fl);
            if (wk[k4] > 0) total += wk[k4] * r;
        }
        return total * 64.0;  // GI_LOAD_SCALE
    }
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        if (fp.weight[k] <= 0) continue;
        bool seen = false;
        [unroll] for (uint j = 0; j < k; ++j) seen = seen || (fp.weight[j] > 0 && frame[j].y == frame[k].y);
        if (seen) continue;
        float w = fp.weight[k];
        [unroll] for (uint j = k + 1; j < 4; ++j)
            if (fp.weight[j] > 0 && frame[j].y == frame[k].y) w += fp.weight[j];
        // 'precise': the map coordinate must not depend on how the compiler fuses this arithmetic in a given kernel (the
        // hardware bilinear quantises it to 1/256 of a texel, so an ulp can move a tap): screenProbeGatherTile equals
        // screenProbeGather bit for bit.
        precise const float3 n = giProbeOctDecode(frame[k].x);
        const uint2 block = uint2(frame[k].y & 0xFFFFu, frame[k].y >> 16);
        const float sgn = n.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017 basis (GiCache.hlsli giBasis)
        precise const float a = -1.0 / (sgn + n.z);
        precise const float c = n.x * n.y * a;
        precise const float3 tb = float3(1 + sgn * n.x * n.x * a, sgn * c, -sgn * n.x);
        precise const float3 bb = float3(c, sgn + n.y * n.y * a, -n.y);
        precise const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
        precise const float3 v = local / (abs(local.x) + abs(local.y) + local.z);
        precise const float2 uv = float2(v.x + v.y, v.x - v.y) * 0.5 + 0.5;
        float3 r = giProbeMapBilinearFrom(t, block, l0, uv);  // in-block maps (the atlas case returned above)
        if (fl > 0 && l1 != l0) r = lerp(r, giProbeMapBilinearFrom(t, block, l1, uv), fl);
        sum += w * r;
    }
    return sum * 64.0;  // GI_LOAD_SCALE
}

// K path by the lobe (INTERFACES 5.6 v1.49, request of A 2026-09-26): the BRDF-weighted mean incident radiance of a GGX
// lobe, L = (integral of L f cos) / (integral of f cos), which M multiplies by its specular albedo S(NoV, roughness)
// as before. The single cone lookup along the mirror direction (split sum) read 8-18 % low against the ray-traced G path
// where the light is not uniform over the lobe (a wall between bright ground and dark sky at a grazing view: the lobe's
// mass is not around the mirror direction) [measured, D0 Wall N], and the K/G boundary showed as an arc.
// Deterministic quadrature: GI_LOBE_TAPS fixed Hammersley points through the visible-normal distribution of the view
// (Dupuy & Benyoub 2023 spherical caps), each tap weighted by G2 / G1(V) (height-correlated Smith), read at the map mip
// whose texel cone matches the tap's share of the lobe's solid angle (1 / (taps x pdf)); taps below the horizon carry no
// weight. Tap count from the quadrature error of a sharp horizon in the field (ground below, sky above: a wall), blurred
// as the 8 x 8 maps blur it, GGX alpha 0.25 [model, scratch lobe_quad.py]: 8 taps 5-8 % mean |error|, 16 taps 1.5-3 %
// (worst ~6 %), where the single cone lookup errs 12-15 % at NoV 0.15-0.3 (worst 17-23 %). Measured (GiAnalytic, a black
// wall above lit ground, alpha 0.25): grazing view 4.2 % mean |error| against the split sum's 11.5 % (NoV < 0.2: +4 %
// against -35 % signed); head-on 5.1 % against 2.2 %. The rest is the 8 x 8 maps' resolution: a lobe about one texel wide
// over a sharp boundary is not integrated exactly from texel averages by any lookup. Fresnel's variation across the lobe is left out of the weights
// (it is in S). Same points for every pixel: no noise. Cost: GI_LOBE_TAPS footprint lookups per K pixel.
#define GI_LOBE_TAPS 16u
float giSmithLambda(float cosTheta, float alpha2)
{
    const float c2 = max(cosTheta * cosTheta, 1e-8);
    return 0.5 * (sqrt(1 + alpha2 * (1 - c2) / c2) - 1);
}
template <typename Src>
float3 giProbeFootprintLobe(Src t, GiProbeFootprint fp, int2 count, float3 n, float3 v, float alpha, uint atlasSrv)
{
    float3 tb, bb;
    giBasis(n, tb, bb);
    const float3 ve = normalize(float3(dot(v, tb), dot(v, bb), max(dot(v, n), 1e-4)));
    const float a = clamp(alpha, 1e-3, 1.0), a2 = a * a;
    const float lambdaV = giSmithLambda(ve.z, a2);
    const float3 vh = normalize(float3(a * ve.x, a * ve.y, ve.z));
    float3 sum = 0;
    float weightSum = 0;
    [loop] for (uint i = 0; i < GI_LOBE_TAPS; ++i)
    {
        const float2 u = float2((i + 0.5) / GI_LOBE_TAPS, reversebits(i) * 2.3283064365386963e-10 + 0.5 / GI_LOBE_TAPS);
        // Visible normal: a point on the spherical cap below vh, then back to the ellipsoid's normal.
        const float phi = 2 * GI_PI * u.x;
        const float z = (1 - u.y) * (1 + vh.z) - vh.z;
        const float sinTheta = sqrt(saturate(1 - z * z));
        const float3 hh = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + vh;
        const float3 m = normalize(float3(a * hh.x, a * hh.y, max(hh.z, 1e-6)));
        const float3 l = 2 * dot(ve, m) * m - ve;
        if (l.z <= 0) continue;
        const float w = (1 + lambdaV) / (1 + lambdaV + giSmithLambda(l.z, a2));
        // pdf(l) = G1(V) D(m) / (4 NoV); the tap's cone has the solid angle 1 / (taps pdf).
        const float d = m.z * m.z * (a2 - 1) + 1;
        const float pdf = (a2 / (GI_PI * d * d)) / ((1 + lambdaV) * 4 * ve.z);
        const float omega = 1 / (GI_LOBE_TAPS * max(pdf, 1e-6));
        // Read one mip level coarser than the tap's own share (cone x 2): of x1, x2, x3 the smallest error at both views of
        // GiAnalytic's lobe check [measured].
        const float cone = 2 * acos(saturate(1 - omega / (2 * GI_PI)));
        sum += w * giProbeFootprintRadiance(t, fp, count, tb * l.x + bb * l.y + n * l.z, cone, atlasSrv);
        weightSum += w;
    }
    return weightSum > 0 ? sum / weightSum : 0;
}

// Incident radiance (nits) from 'dir' prefiltered by a cone of half-angle coneHalfAngle (radians): the K reflection path
// (INTERFACES 5.6 v1.2). Same four probes and weights as screenProbeIrradiance; per probe the map mip whose texel cone
// matches the lobe with the design's Nyquist margin 1.5 (8 x 8 texel ~10.1 deg, 4 x 4 ~20.2, 2 x 2 ~40.4 half-angle),
// trilinear between mips. Directions below a probe map's hemisphere use its horizon texels.
float3 screenProbeRadiance(ProbeSrvs s, uint2 pixel, float3 normal, float linearDepth, float3 dir, float coneHalfAngle)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    float spacing;
    int2 count;
    const GiProbeFootprint fp = giProbeFootprint(t, pixel, normal, linearDepth, spacing, count);
    return giProbeFootprintRadiance(t, fp, count, dir, coneHalfAngle);
}
// Everything M's shading takes from the screen probes in one footprint (v1.13, request 20260925_R_probe_lookup_structure):
// irradiance on the normal and near occlusion (as screenProbeIrradiance), optionally the irradiance of the back side
// (-normal, its own footprint: Foliage transmission; M applies the front occlusion as before), and optionally the K-path
// radiance (as screenProbeRadiance, from the hardware-filtered atlas). s = { view.screenProbes SRV, (same), view.screenProbeMaps
// SRV, 0 }; worldPos = the pixel's surface point (M has it; no inverse projection here).
struct ScreenProbeLighting
{
    float3 irradiance;
    float occlusion;
    float3 irradianceBack;
    float3 radiance;
};

template <typename Src>
float4 giFootprintIrradiance(Src t, GiProbeFootprint fp, int2 count, float3 normal)
{
    float4 sum = float4(0, 0, 0, 0);
    bool any = false;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        if (fp.weight[k] <= 0) continue;
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(fp.probe[k]), count);
        sum += fp.weight[k] * float4(giEvalShIrradiance(r.sh, normal), r.occlusion);
        any = true;
    }
    return any ? sum : float4(0, 0, 0, 1);
}

template <typename Src>
ScreenProbeLighting giProbeGatherFrom(Src t, ProbeSrvs s, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, bool back, bool wantRadiance,
                                      float3 dir, float coneHalfAngle, float lobeAlpha = -1)
{
    float spacing;
    int2 count;
    const GiProbeFootprint fp = giProbeFootprintAt(t, pixel, worldPos, normal, linearDepth, spacing, count);
    ScreenProbeLighting o;
    const float4 e = giFootprintIrradiance(t, fp, count, normal);
    o.irradiance = e.rgb;
    o.occlusion = e.a;
    o.irradianceBack = 0;
    if (back)
    {
        const GiProbeFootprint fb = giProbeFootprintAt(t, pixel, worldPos, -normal, linearDepth, spacing, count);
        o.irradianceBack = giFootprintIrradiance(t, fb, count, -normal).rgb;
    }
    if (s.pad1 != 0)
    {
        // The cache's irradiance maps at this pixel (global loads; the tile LDS path is step 2 of revision D).
        ByteAddressBuffer cache = ResourceDescriptorHeap[s.pad1 - 1];
        const GiHeader h = giHeader(cache);
        float weight;
        const float3 front = giCacheIrradianceScreen(cache, h, worldPos, normal, weight);
        if (weight > 0) o.irradiance = front;
        if (back)
        {
            const float3 behind = giCacheIrradianceScreen(cache, h, worldPos, -normal, weight);
            if (weight > 0) o.irradianceBack = behind;
        }
    }
    if (!wantRadiance) o.radiance = 0;
    else if (lobeAlpha >= 0) o.radiance = giProbeFootprintLobe(t, fp, count, normal, dir, lobeAlpha, s.pad0);  // dir = V
    else o.radiance = giProbeFootprintRadiance(t, fp, count, dir, coneHalfAngle, s.pad0);
    return o;
}

ScreenProbeLighting screenProbeGather(ProbeSrvs s, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, bool back, bool wantRadiance, float3 dir,
                                      float coneHalfAngle)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    return giProbeGatherFrom(t, s, pixel, worldPos, normal, linearDepth, back, wantRadiance, dir, coneHalfAngle);
}
// As screenProbeGather with the K radiance as the lobe's BRDF-weighted mean (giProbeFootprintLobe, v1.49): v = the unit
// vector from the surface to the eye, alpha = GGX alpha (perceptual roughness squared); M multiplies it by S as before.
ScreenProbeLighting screenProbeGatherLobe(ProbeSrvs s, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, bool back, bool wantRadiance, float3 v,
                                          float alpha)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    return giProbeGatherFrom(t, s, pixel, worldPos, normal, linearDepth, back, wantRadiance, v, 0, max(alpha, 0.0));
}

#ifdef GI_PROBE_TILE_CACHE
// screenProbeGather for a pixel of 'tile' (its 8 x 8 group) after giProbeTileLoad and the group barrier.
ScreenProbeLighting screenProbeGatherTile(ProbeSrvs s, uint2 tile, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, bool back, bool wantRadiance,
                                          float3 dir, float coneHalfAngle)
{
    GiProbeTile t;
    t.first = int2(tile);
    return giProbeGatherFrom(t, s, pixel, worldPos, normal, linearDepth, back, wantRadiance, dir, coneHalfAngle);
}
// screenProbeGatherLobe for a pixel of 'tile' (its 8 x 8 group) after giProbeTileLoad and the group barrier.
ScreenProbeLighting screenProbeGatherLobeTile(ProbeSrvs s, uint2 tile, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, bool back, bool wantRadiance,
                                              float3 v, float alpha)
{
    GiProbeTile t;
    t.first = int2(tile);
    return giProbeGatherFrom(t, s, pixel, worldPos, normal, linearDepth, back, wantRadiance, v, 0, max(alpha, 0.0));
}
#endif

#endif
