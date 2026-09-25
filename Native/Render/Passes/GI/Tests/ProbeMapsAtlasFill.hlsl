// unx-kernel: cs_6_6 main
// Probe maps atlas test (ProbeMapsAtlas.cpp): one thread per probe writes the same random RGB9E5 radiance maps (8 x 8,
// 4 x 4, 2 x 2) into the in-block layout of the probes texture and into the atlas (ScreenProbes.hlsli).
// P[0] = { probes UAV (RGBA32_UINT), atlas UAV (R32_UINT), probesX, probesY }
#include "Passes/GI/GiInternal.hlsli"

uint hashTest(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float unitTest(uint x) { return (hashTest(x) >> 8) * (1.0 / 16777216.0); }

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    if (probe.x >= P[0].z || probe.y >= P[0].w) return;
    RWTexture2D<uint4> t = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> atlas = ResourceDescriptorHeap[P[0].y];
    const uint x = probe.x * 8, y = probe.y * 4, seed = (probe.y * P[0].z + probe.x) * 97u;
    uint packed[84];
    [loop] for (uint k = 0; k < 84; ++k)
    {
        // Values over several decades (the shared exponent) with independent channels.
        const float3 c = float3(unitTest(seed + 3 * k), unitTest(seed + 3 * k + 1), unitTest(seed + 3 * k + 2)) * exp2(unitTest(seed + 1000 + k) * 12 - 6);
        packed[k] = giPackRgb9e5(c);
    }
    // In-block layout: level 0 rows 1-2 (4 per texel, row-major), level 1 row 3 texels 0-3, level 2 row 0 texel 4.
    [loop] for (uint q = 0; q < 16; ++q) t[uint2(x + q % 8, y + 1 + q / 8)] = uint4(packed[4 * q], packed[4 * q + 1], packed[4 * q + 2], packed[4 * q + 3]);
    [loop] for (uint r = 0; r < 4; ++r) t[uint2(x + r, y + 3)] = uint4(packed[64 + 4 * r], packed[64 + 4 * r + 1], packed[64 + 4 * r + 2], packed[64 + 4 * r + 3]);
    t[uint2(x + 4, y)] = uint4(packed[80], packed[81], packed[82], packed[83]);
    // Atlas: level L tile at (x0_L + i n, j n).
    [loop] for (uint k0 = 0; k0 < 64; ++k0) atlas[uint2(probe.x * 8 + k0 % 8, probe.y * 8 + k0 / 8)] = packed[k0];
    [loop] for (uint k1 = 0; k1 < 16; ++k1) atlas[uint2(8 * P[0].z + probe.x * 4 + k1 % 4, probe.y * 4 + k1 / 4)] = packed[64 + k1];
    [loop] for (uint k2 = 0; k2 < 4; ++k2) atlas[uint2(12 * P[0].z + probe.x * 2 + k2 % 2, probe.y * 2 + k2 / 2)] = packed[80 + k2];
}
