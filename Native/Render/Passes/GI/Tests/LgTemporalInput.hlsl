// unx-kernel: cs_6_6 main
#include "Passes/GI/Lumen/LgSurface.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy;
    if (any(p >= size)) return;
    const uint seed = P[3].z, mode = P[3].w, i = p.x + p.y * size.x;
    uint h = (i + seed * 977u) * 1664525u + 1013904223u; h ^= h >> 16;
    const float3 c = float3(h & 1023u, (h >> 10) & 1023u, (h >> 20) & 1023u) / 1023.0;
    const bool sky = i % 23u == 0 || (p.x / 8 + p.y / 8) % 19u == 0;
    const float z = sky ? 0 : g_nearPlane / (4 + (p.x / 13u) % 3u);
    GBufferSample gb; gb.normal = normalize(float3(c.xy * .25 - .125, 1)); gb.baseColor = c; gb.roughness = .4;
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].x]; depth[p] = z;
    RWTexture2D<uint2> buffer = ResourceDescriptorHeap[P[0].y]; buffer[p] = encodeGBuffer(gb);
    RWTexture2D<float4> fresh = ResourceDescriptorHeap[P[0].z], spec = ResourceDescriptorHeap[P[0].w], back = ResourceDescriptorHeap[P[1].x];
    const float scale = seed & 1u ? 32 : .01;
    fresh[p] = float4(c * scale, i % 29u == 0 ? -.01 : mode == 3 ? .09 : .005);
    spec[p] = float4(c.gbr * scale, 0); back[p] = float4(c.bgr * scale, 1);
    RWTexture2D<float4> pd = ResourceDescriptorHeap[P[1].y], ps = ResourceDescriptorHeap[P[1].z], pb = ResourceDescriptorHeap[P[2].x];
    pd[p] = float4(c.bgr * scale, mode == 2 ? 8 : 2); ps[p] = float4(c.grb * scale, 0); pb[p] = float4(c.gbr * scale, 1);
    RWTexture2D<uint2> keys = ResourceDescriptorHeap[P[1].w];
    const float2 e = lgEncodeNormal(gb.normal);
    keys[p] = uint2(asuint(i % 31u == 0 ? z * .1 : z), (uint)round(e.x * 32767) | ((uint)round(e.y * 32767) << 15));
    RWTexture2D<uint> word = ResourceDescriptorHeap[P[2].y]; word[p] = i & 1u;
}
