// unx-kernel: cs_6_6 main
// Water foam F (Foam.hlsli), once per sea state: per foam level l, the divergence variance of the displacement bands a
// texel s_l removes, sigma_J^2 = sum over bins with |k| > pi / s_l of |k|^2 (|h0(k)|^2 + |h0(-k)|^2) (Parseval over the
// realised spectrum; the time average of |h(k, t)|^2). Summed per wave, then added as 64-bit fixed point (2^-40).
// P[0] h0 SRV (raw, float4 per bin: h0(k), conj(h0(-k)); Ocean.hlsli), accumulator UAV (raw, uint64 per level), levels,
// 0; P[1] s_0 (m); P[2] cascade lengths (Ocean.hlsli oceanLength)
#include "Ocean.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    float w = 0, k = 0;
    if (i < OCEAN_N * OCEAN_N * 3)
    {
        const uint cascade = i / (OCEAN_N * OCEAN_N), bin = i % (OCEAN_N * OCEAN_N);
        ByteAddressBuffer h0 = ResourceDescriptorHeap[P[0].x];
        const float4 h = asfloat(h0.Load4(16 * i));
        const float2 kv = oceanK(uint2(bin % OCEAN_N, bin / OCEAN_N), oceanLength(cascade));
        k = length(kv);
        w = dot(kv, kv) * (dot(h.xy, h.xy) + dot(h.zw, h.zw));
    }
    RWByteAddressBuffer accumulator = ResourceDescriptorHeap[P[0].y];
    const float s0 = asfloat(P[1].x);
    for (uint level = 0; level < P[0].z; ++level)
    {
        const float cut = OCEAN_PI / (s0 * float(1u << (2 * level)));
        const float sum = WaveActiveSum(k > cut ? w : 0.0);
        if (WaveIsFirstLane() && sum > 0) accumulator.InterlockedAdd64(8 * level, uint64_t(sum * 1099511627776.0 + 0.5));
    }
}
