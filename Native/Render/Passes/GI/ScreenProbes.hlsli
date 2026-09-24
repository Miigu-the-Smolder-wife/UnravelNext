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
// Texture: RGBA32_UINT, (probesX * 8) x (probesY * 4 + 1). Probe (i, j) is the block of texels (8i .. 8i+7, 4j .. 4j+3):
//   row 0: texels 0-3 = record (27 fp16 irradiance SH coefficients x GI_STORE_SCALE + fp16 linear depth, octahedral
//          normal, occlusion unorm16 | pixel offset in the tile (3+3 bits) | valid), texel 4 = the 2 x 2 mip (RGB9E5 x 4)
//   rows 1-2: the 8 x 8 map, 4 RGB9E5 texels per RGBA32 texel, row-major
//   row 3: texels 0-3 = the 4 x 4 mip, texel 4.x = the map's frame normal (octahedral)
// Last row, texel 0 = { spacing px, probesX, probesY, 0 }. Radiance values are x GI_STORE_SCALE (1/64).
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
    float linearDepth;
    float3 normal;
    float occlusion;
    uint2 offset;  // probe pixel within its tile
    bool valid;
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
    const float scale = exp2((float)(v >> 27) - 24.0);  // bias 15, 9-bit mantissa
    return float3(v & 0x1FFu, (v >> 9) & 0x1FFu, (v >> 18) & 0x1FFu) * scale;
}

GiProbeRecord giLoadProbeRecord(Texture2D<uint4> t, uint2 probe)
{
    const uint x = probe.x * 8, y = probe.y * 4;
    const uint4 a = t.Load(int3(x, y, 0)), b = t.Load(int3(x + 1, y, 0)), c = t.Load(int3(x + 2, y, 0)), d = t.Load(int3(x + 3, y, 0));
    const uint w[16] = { a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, d.x, d.y, d.z, d.w };
    GiProbeRecord r;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const uint i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        r.sh[k] = float3(f16tof32(w[i0 >> 1] >> ((i0 & 1) * 16)), f16tof32(w[i1 >> 1] >> ((i1 & 1) * 16)), f16tof32(w[i2 >> 1] >> ((i2 & 1) * 16))) * 64.0;  // GI_LOAD_SCALE
    }
    r.linearDepth = f16tof32(w[13] >> 16);
    r.normal = giProbeOctDecode(w[14]);
    r.occlusion = (w[15] & 0xFFFFu) / 65535.0;
    r.offset = uint2((w[15] >> 16) & 7u, (w[15] >> 19) & 7u);
    r.valid = (w[15] & (1u << 22)) != 0;
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

// Radiance texel (tx, ty) of mip 'level' (0 = 8 x 8, 1 = 4 x 4, 2 = 2 x 2) of a probe, x GI_STORE_SCALE.
float3 giProbeMapTexel(Texture2D<uint4> t, uint2 probe, uint level, uint2 texel)
{
    const uint x = probe.x * 8, y = probe.y * 4;
    uint index, packed;
    if (level == 0)
    {
        index = texel.y * 8 + texel.x;
        const uint4 v = t.Load(int3(x + (index >> 2) % 8, y + 1 + (index >> 5), 0));
        packed = v[index & 3];
    }
    else if (level == 1)
    {
        index = texel.y * 4 + texel.x;
        const uint4 v = t.Load(int3(x + (index >> 2), y + 3, 0));
        packed = v[index & 3];
    }
    else
    {
        index = texel.y * 2 + texel.x;
        const uint4 v = t.Load(int3(x + 4, y, 0));
        packed = v[index & 3];
    }
    return giUnpackRgb9e5(packed);
}

// Bilinear radiance of a probe's map at mip 'level' for hemispherical octahedral coordinates uv.
float3 giProbeMapBilinear(Texture2D<uint4> t, uint2 probe, uint level, float2 uv)
{
    const uint n = 8u >> level;
    const float2 x = uv * n - 0.5;
    const int2 i0 = int2(floor(x));
    const float2 f = x - floor(x);
    float3 r = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const uint2 tx = uint2(clamp(i0 + o, 0, int(n) - 1));
        r += ((o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y)) * giProbeMapTexel(t, probe, level, tx);
    }
    return r;
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
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(p));
        if (!r.valid) continue;
        const float2 probePixel = float2(p) * spacing + float2(r.offset);
        const float3 probeWorld = worldFromDepth(probePixel, g_nearPlane / max(r.linearDepth, 1e-6));
        const float plane = abs(dot(normal, probeWorld - pixelWorld)) / max(linearDepth, 1e-6);
        const float wPlane = pow(saturate(1 - plane / 0.02), 2);
        const float wNormal = pow(saturate(dot(normal, r.normal)), 4);
        fp.weight[k] = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * wPlane * wNormal;
        total += fp.weight[k];
    }
    if (total < 1e-4)
    {
        const int2 own = clamp(int2(pixel / (uint)spacing), int2(0, 0), count - 1);
        fp.probe[0] = own;
        fp.weight[0] = giLoadProbeRecord(t, uint2(own)).valid ? 1 : 0;
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
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(fp.probe[k]));
        sum += fp.weight[k] * float4(giEvalShIrradiance(r.sh, normal), r.occlusion);
        any = true;
    }
    return any ? sum : float4(0, 0, 0, 1);
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
    const float lod = clamp(log2(max(coneHalfAngle, 1e-3) / (1.5 * 0.1763)), 0.0, 2.0);  // 0.1763 rad = 10.1 deg
    const uint l0 = (uint)floor(lod), l1 = min(l0 + 1, 2u);
    const float fl = lod - l0;
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        if (fp.weight[k] <= 0) continue;
        const uint2 p = uint2(fp.probe[k]);
        const float3 n = giProbeOctDecode(t.Load(int3(p.x * 8 + 4, p.y * 4 + 3, 0)).x);
        const float sgn = n.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017 basis (GiCache.hlsli giBasis)
        const float a = -1.0 / (sgn + n.z);
        const float c = n.x * n.y * a;
        const float3 tb = float3(1 + sgn * n.x * n.x * a, sgn * c, -sgn * n.x);
        const float3 bb = float3(c, sgn + n.y * n.y * a, -n.y);
        const float3 local = float3(dot(dir, tb), dot(dir, bb), max(dot(dir, n), 0.0));
        const float3 v = local / (abs(local.x) + abs(local.y) + local.z);
        const float2 uv = float2(v.x + v.y, v.x - v.y) * 0.5 + 0.5;
        const float3 r0 = giProbeMapBilinear(t, p, l0, uv), r1 = giProbeMapBilinear(t, p, l1, uv);
        sum += fp.weight[k] * lerp(r0, r1, fl);
    }
    return sum * 64.0;  // GI_LOAD_SCALE
}

#endif
