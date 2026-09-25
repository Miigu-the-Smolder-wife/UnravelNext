// Screen probes (R track, ARCHITECTURE 2.5, 2.6; INTERFACES 5.6 public API: ProbeSrvs, screenProbeIrradiance,
// screenProbeRadiance). Consumer: M's shading kernel (main view). One probe per gi.screen_probe_spacing_px square tile,
// filled every frame from the world radiance cache (no rays):
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
// Last row (5 probesY), texel 0 = { spacing px, probesX, probesY, 0 }. Radiance values are x GI_STORE_SCALE (1/64).
#ifndef UNX_GI_SCREENPROBES_HLSLI
#define UNX_GI_SCREENPROBES_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

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

// The footprint's part of a probe record: world position and normal; false when the probe has no surface.
bool giLoadProbeSurface(Texture2D<uint4> t, uint2 probe, int2 count, out float3 position, out float3 normal)
{
    const uint4 a = t.Load(int3(probe.x, count.y * 4 + probe.y, 0));
    position = asfloat(a.xyz);
    normal = giProbeOctDecode(a.w);
    return a.w != 0;
}

GiProbeRecord giLoadProbeRecord(Texture2D<uint4> t, uint2 probe, int2 count)
{
    const int y = count.y * 4 + probe.y;
    const uint4 a = t.Load(int3(probe.x + count.x, y, 0)), b = t.Load(int3(probe.x + 2 * count.x, y, 0)), c = t.Load(int3(probe.x + 3 * count.x, y, 0)),
                d = t.Load(int3(probe.x + 4 * count.x, y, 0));
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

GiProbeFootprint giProbeFootprint(Texture2D<uint4> t, uint2 pixel, float3 normal, float linearDepth, out float spacing, out int2 count)
{
    uint width, height;
    t.GetDimensions(width, height);
    const uint4 header = t.Load(int3(0, height - 1, 0));
    spacing = (float)header.x;
    count = int2(header.y, header.z);
    const float2 f = (float2(pixel) + 0.5) / spacing - 0.5;
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    const float3 pixelWorld = worldFromDepth(float2(pixel), g_nearPlane / max(linearDepth, 1e-6));
    GiProbeFootprint fp;
    float total = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 p = clamp(i0 + o, int2(0, 0), count - 1);
        fp.probe[k] = p;
        fp.weight[k] = 0;
        float3 probeWorld, probeNormal;
        if (!giLoadProbeSurface(t, uint2(p), count, probeWorld, probeNormal)) continue;
        const float plane = saturate(1 - abs(dot(normal, probeWorld - pixelWorld)) / max(linearDepth, 1e-6) / 0.02);
        const float agree = saturate(dot(normal, probeNormal));
        const float agree2 = agree * agree;
        fp.weight[k] = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * (plane * plane) * (agree2 * agree2);
        total += fp.weight[k];
    }
    if (total < 1e-4)
    {
        const int2 own = clamp(int2(pixel / (uint)spacing), int2(0, 0), count - 1);
        fp.probe[0] = own;
        float3 ownWorld, ownNormal;
        fp.weight[0] = giLoadProbeSurface(t, uint2(own), count, ownWorld, ownNormal) ? 1 : 0;
        fp.weight[1] = fp.weight[2] = fp.weight[3] = 0;
        total = fp.weight[0];
    }
    [unroll] for (uint j = 0; j < 4; ++j) fp.weight[j] = total > 0 ? fp.weight[j] / total : 0;
    return fp;
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

// The K path over a footprint. Probes that read the same cache entry share one map block (GiProbeMapOwners): their
// weights are merged and each distinct block is looked up once. The second mip is read only when it differs from the
// first (fractional level below the top mip).
float3 giProbeFootprintRadiance(Texture2D<uint4> t, GiProbeFootprint fp, int2 count, float3 dir, float coneHalfAngle)
{
    const float lod = clamp(log2(max(coneHalfAngle, 1e-3) / (1.5 * 0.1763)), 0.0, 2.0);  // 0.1763 rad = 10.1 deg
    const uint l0 = (uint)floor(lod), l1 = min(l0 + 1, 2u);
    const float fl = lod - l0;
    uint2 frame[4];
    [unroll] for (uint k = 0; k < 4; ++k)
        frame[k] = fp.weight[k] > 0 ? t.Load(int3(fp.probe[k].x + 5 * count.x, count.y * 4 + fp.probe[k].y, 0)).xy : uint2(0, 0xFFFFFFFFu);
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
        const float3 n = giProbeOctDecode(frame[k].x);
        const uint2 block = uint2(frame[k].y & 0xFFFFu, frame[k].y >> 16);
        const float sgn = n.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017 basis (GiCache.hlsli giBasis)
        const float a = -1.0 / (sgn + n.z);
        const float c = n.x * n.y * a;
        const float3 tb = float3(1 + sgn * n.x * n.x * a, sgn * c, -sgn * n.x);
        const float3 bb = float3(c, sgn + n.y * n.y * a, -n.y);
        const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
        const float3 v = local / (abs(local.x) + abs(local.y) + local.z);
        const float2 uv = float2(v.x + v.y, v.x - v.y) * 0.5 + 0.5;
        float3 r = giProbeMapBilinear(t, block, l0, uv);
        if (fl > 0 && l1 != l0) r = lerp(r, giProbeMapBilinear(t, block, l1, uv), fl);
        sum += w * r;
    }
    return sum * 64.0;  // GI_LOAD_SCALE
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

#endif
