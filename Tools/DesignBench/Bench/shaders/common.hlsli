// Shared root layout for every DesignBench kernel: b0 = 32 root constants (uint4 P[8]), static samplers s0 (point clamp)
// and s1 (linear clamp), every other resource through ResourceDescriptorHeap (SM 6.6). Every loop in these kernels has
// a constant upper bound (the WARP dry run must terminate).
#ifndef DB_COMMON_HLSLI
#define DB_COMMON_HLSLI

cbuffer Root : register(b0) { uint4 P[8]; }
SamplerState g_point : register(s0);
SamplerState g_linear : register(s1);

uint pcg(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float u01(inout uint s) { s = pcg(s); return (s >> 8) * (1.0 / 16777216.0); }

// Octahedral normal encoding (snorm16 x 2), as GBuffer.hlsli.
uint octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * select(n.xy >= 0, 1.0, -1.0);
    int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16);
}
float3 octDecode(uint packed)
{
    float2 e = float2(int2(packed << 16, packed) >> 16) / 32767.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0, 1.0, -1.0);
    return normalize(n);
}

#endif
