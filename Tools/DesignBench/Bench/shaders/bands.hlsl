// Bench 3: banded pixel passes (COVERAGE_REDESIGN_KO.md 4.8).
// ResolveCS: reads vis (4 B) + depth (4 B), writes gbuffer (8 B) + material word (4 B).
// ShadeCS: reads vis, depth, gbuffer, word, shadow, irradiance (RG32: 8 B) = 32 B (+ EXTRA_READS x 8 B), writes RGBA8.
// Both dispatch over a row band [y0, y1): P[0] = { width, height, y0, y1 }; full screen = one band.
// P[1] = { vis, depth, gbuffer, word }, P[2] = { shadow, irradiance, output, extra0 }, P[3] = { extra1, extra2, -, - }
#include "common.hlsli"
#ifndef EXTRA_READS
#define EXTRA_READS 0
#endif

[numthreads(8, 8, 1)]
void ResolveCS(uint2 id : SV_DispatchThreadID)
{
    const uint2 pixel = uint2(id.x, id.y + P[0].z);
    if (pixel.x >= P[0].x || pixel.y >= P[0].w) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].z];
    RWTexture2D<uint> word = ResourceDescriptorHeap[P[1].w];
    const uint v = vis[pixel];
    const float z = depth[pixel];
    // Some ALU standing in for the surface reconstruction: a normal from the id and depth gradients, a colour.
    const float3 n = normalize(float3(sin(v * 0.001 + z), cos(v * 0.0007), 0.5 + 0.5 * frac(z * 13.0)));
    const uint col = (uint)(frac(v * 0.013) * 255) | ((uint)(frac(z * 7.0) * 255) << 8) | ((uint)(frac(v * 0.021) * 255) << 16) | (((v >> 3) & 0xFFu) << 24);
    gbuffer[pixel] = uint2(octEncode(n), col);
    word[pixel] = (v & 0xFFFFu) | ((uint)(frac(z * 3.0) * 255) << 16);
}

[numthreads(8, 8, 1)]
void ShadeCS(uint2 id : SV_DispatchThreadID)
{
    const uint2 pixel = uint2(id.x, id.y + P[0].z);
    if (pixel.x >= P[0].x || pixel.y >= P[0].w) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[1].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].z];
    Texture2D<uint> word = ResourceDescriptorHeap[P[1].w];
    Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
    Texture2D<uint2> irradiance = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[2].z];
    const uint v = vis[pixel];
    const float z = depth[pixel];
    const uint2 g = gbuffer[pixel];
    const uint w = word[pixel];
    const uint sh = shadow[pixel];
    const uint2 ir = irradiance[pixel];
    float3 sum = 0;
#if EXTRA_READS >= 1
    { Texture2D<uint2> e = ResourceDescriptorHeap[P[2].w]; const uint2 t = e[pixel]; sum += float3(t.x & 255u, t.y & 255u, 1) * 1e-4; }
#endif
#if EXTRA_READS >= 2
    { Texture2D<uint2> e = ResourceDescriptorHeap[P[3].x]; const uint2 t = e[pixel]; sum += float3(t.x & 255u, t.y & 255u, 1) * 1e-4; }
#endif
#if EXTRA_READS >= 3
    { Texture2D<uint2> e = ResourceDescriptorHeap[P[3].y]; const uint2 t = e[pixel]; sum += float3(t.x & 255u, t.y & 255u, 1) * 1e-4; }
#endif
    const float3 n = octDecode(g.x);
    const float3 albedo = float3(g.y & 255u, (g.y >> 8) & 255u, (g.y >> 16) & 255u) / 255.0;
    const float rough = ((w >> 16) & 255u) / 255.0;
    const float3 l = normalize(float3(0.3, 0.8, 0.5)), vdir = float3(0, 0, 1), h = normalize(l + vdir);
    const float ndl = saturate(dot(n, l)), ndh = saturate(dot(n, h));
    const float a2 = max(rough * rough, 1e-3);
    const float d = a2 / (3.14159 * pow(ndh * ndh * (a2 - 1) + 1, 2));
    const float visSun = (sh & 255u) / 255.0;
    const float3 E = float3(f16tof32(ir.x), f16tof32(ir.x >> 16), f16tof32(ir.y));
    sum += (albedo / 3.14159 * ndl + 0.04 * d * 0.25) * visSun * 3.0 + albedo * E + (v == 0 ? float3(0.3, 0.5, 0.9) : 0) * z;
    output[pixel] = float4(sum / (1 + sum), 1);
}
