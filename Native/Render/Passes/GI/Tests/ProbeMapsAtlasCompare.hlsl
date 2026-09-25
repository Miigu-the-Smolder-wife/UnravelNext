// unx-kernel: cs_6_6 main
// Probe maps atlas test (ProbeMapsAtlas.cpp): random (probe, level, uv) samples, uv in [-0.25, 1.25] so clamping is
// exercised, through the software in-block bilinear (giProbeMapBilinear) and the hardware-filtered atlas
// (giProbeAtlasBilinear). Per sample the difference relative to the largest of its four texels (the scale of a bilinear
// weight error) goes into the maximum (uint bits of a positive float) and a count above 1/128.
// P[0] = { probes SRV, atlas SRV (R9G9B9E5), probesX, probesY }, P[1] = { result UAV (raw: max bits, count), samples, 0, 0 }
#include "Passes/GI/ScreenProbes.hlsli"

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

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].y) return;
    Texture2D<uint4> t = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> atlas = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[1].x];
    const int2 count = int2(P[0].z, P[0].w);
    const uint2 probe = uint2(hashTest(i * 7 + 1) % P[0].z, hashTest(i * 7 + 2) % P[0].w);
    const uint level = hashTest(i * 7 + 3) % 3;
    const float2 uv = float2(unitTest(i * 7 + 4), unitTest(i * 7 + 5)) * 1.5 - 0.25;
    const float3 sw = giProbeMapBilinear(t, probe, level, uv);
    const float3 hw = giProbeAtlasBilinear(atlas, count, probe, level, uv);
    // Scale: the largest texel of the level's tile around uv (bounds every bilinear weight error).
    const uint n = 8u >> level;
    const int2 i0 = int2(floor(uv * n - 0.5));
    float scale = 1e-30;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 tx = uint2(clamp(i0 + int2(k & 1, k >> 1), 0, int(n) - 1));
        const float3 v = giProbeMapBilinear(t, probe, level, (float2(tx) + 0.5) / n);
        scale = max(scale, max(v.r, max(v.g, v.b)));
    }
    const float3 d = abs(hw - sw) / scale;
    const float e = max(d.r, max(d.g, d.b));
    uint previous;
    result.InterlockedMax(0, asuint(e), previous);
    if (e > 1.0 / 128) result.InterlockedAdd(4, 1u, previous);
}
