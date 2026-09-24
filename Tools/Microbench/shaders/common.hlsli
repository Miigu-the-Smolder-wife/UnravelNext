// Shared root layout and helpers for every microbench kernel.
// Root signature: b0 = 32 root constants (uint4 P[8]), t0 = TLAS (root SRV), u0 = generic output (root UAV).
// All other resources are reached through ResourceDescriptorHeap / SamplerDescriptorHeap (SM 6.6 dynamic resources).
#ifndef MB_COMMON_HLSLI
#define MB_COMMON_HLSLI

cbuffer Root : register(b0) { uint4 P[8]; }

uint pcg(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float u01(inout uint s) { s = pcg(s); return (s >> 8) * (1.0 / 16777216.0); }

float3 sphereDir(inout uint s)
{
    float z = 1.0 - 2.0 * u01(s);
    float a = 6.28318530718 * u01(s);
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), z, r * sin(a));
}

// Same analytic terrain the host uses to build the heightfield (metres).
float terrainHeight(float x, float z)
{
    return 6.0 * sin(x * (1.0 / 70.0)) * cos(z * (1.0 / 90.0))
         + 2.0 * sin(x * (1.0 / 13.0) + z * (1.0 / 17.0))
         + 0.7 * sin(x * (1.0 / 3.1) - z * (1.0 / 2.7));
}

#endif
