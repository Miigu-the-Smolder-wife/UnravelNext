// Screen probes (R track, ARCHITECTURE 2.5; INTERFACES 5.6 public API: ProbeSrvs, screenProbeIrradiance).
// Consumer: M's shading kernel (main view). One probe per gi.screen_probe_spacing_px square tile, filled every frame from
// the world radiance cache (no rays): trilinear L2 SH irradiance (indirect + sky, no direct sun) and the near occlusion of
// the probe's surface (depth-buffer taps within the sub-cell radius, computed at probe resolution).
//
// Use: ProbeSrvs{ c.srv(view.screenProbes), c.srv(view.screenProbes), 0, 0 } (both fields: the one texture; the pass
// declares view.screenProbes SrvCompute) with the main view's frame constants bound (b1).
//
// Texture: RGBA32_UINT, (probesX * 4) x (probesY + 1). Probe (i, j) = texels (4i..4i+3, j), 64 B: 27 fp16 irradiance SH
// coefficients (x 1/64, GiCache.hlsli GI_STORE_SCALE) + fp16 linear depth, octahedral normal, occlusion unorm16 | pixel offset in the tile (3+3 bits) | valid.
// Row probesY, texel 0 = { spacing px, probesX, probesY, 0 }.
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

GiProbeRecord giLoadProbeRecord(Texture2D<uint4> t, uint2 probe)
{
    const uint x = probe.x * 4;
    const uint4 a = t.Load(int3(x, probe.y, 0)), b = t.Load(int3(x + 1, probe.y, 0)), c = t.Load(int3(x + 2, probe.y, 0)), d = t.Load(int3(x + 3, probe.y, 0));
    const uint w[16] = { a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, d.x, d.y, d.z, d.w };
    GiProbeRecord r;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const uint i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        r.sh[k] = float3(f16tof32(w[i0 >> 1] >> ((i0 & 1) * 16)), f16tof32(w[i1 >> 1] >> ((i1 & 1) * 16)), f16tof32(w[i2 >> 1] >> ((i2 & 1) * 16))) * 64.0;  // GI_LOAD_SCALE
    }
    r.linearDepth = f16tof32(w[13] >> 16);
    const float2 e = float2(int2(w[14] << 16, w[14]) >> 16) / 32767.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    r.normal = normalize(n);
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

// rgb = indirect + sky irradiance (lux-equivalent: nits * sr) on a surface with this normal; a = near occlusion (1 = open).
// Interpolates the four surrounding probes, weighted by distance to the pixel's tangent plane and normal agreement;
// falls back to the pixel's own tile probe when none agrees (thin features between probes).
float4 screenProbeIrradiance(ProbeSrvs s, uint2 pixel, float3 normal, float linearDepth)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.probes];
    uint width, height;
    t.GetDimensions(width, height);
    const uint4 header = t.Load(int3(0, height - 1, 0));
    const float spacing = (float)header.x;
    const int2 count = int2(header.y, header.z);
    const float2 f = (float2(pixel) + 0.5) / spacing - 0.5;
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    const float3 pixelWorld = worldFromDepth(float2(pixel), g_nearPlane / max(linearDepth, 1e-6));
    float3 sum = 0;
    float occlusion = 0, weight = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 p = clamp(i0 + o, int2(0, 0), count - 1);
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(p));
        if (!r.valid) continue;
        const float2 probePixel = float2(p) * spacing + float2(r.offset);
        const float3 probeWorld = worldFromDepth(probePixel, g_nearPlane / max(r.linearDepth, 1e-6));
        const float plane = abs(dot(normal, probeWorld - pixelWorld)) / max(linearDepth, 1e-6);
        const float wPlane = pow(saturate(1 - plane / 0.02), 2);
        const float wNormal = pow(saturate(dot(normal, r.normal)), 4);
        const float w = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * wPlane * wNormal;
        if (w <= 0) continue;
        sum += w * giEvalShIrradiance(r.sh, normal);
        occlusion += w * r.occlusion;
        weight += w;
    }
    if (weight < 1e-4)
    {
        const int2 own = clamp(int2(pixel / (uint)spacing), int2(0, 0), count - 1);
        const GiProbeRecord r = giLoadProbeRecord(t, uint2(own));
        return r.valid ? float4(giEvalShIrradiance(r.sh, normal), r.occlusion) : float4(0, 0, 0, 1);
    }
    return float4(sum / weight, occlusion / weight);
}

#endif
