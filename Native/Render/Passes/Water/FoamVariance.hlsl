// unx-kernel: cs_6_6 main
// Water foam F (Foam.hlsli), once per sea state: per foam level l, the divergence variance of the displacement bands a
// texel s_l removes, sigma_J^2 = sum over bins with |k| > pi / s_l of |k|^2 (|h0(k)|^2 + |h0(-k)|^2) (Parseval over the
// realised spectrum; the time average of |h(k, t)|^2). Summed per wave, then added as 64-bit fixed point (2^-40).
// For the automatic breaking threshold (FoamCalibrate): the covariances of the displacement gradient (a, b, c) =
// (dDx/dx, dDz/dz, dDx/dz) over every band, with D = i k / |k| h: per bin a, b, c carry kx^2 / |k|, kz^2 / |k|,
// kx kz / |k| times h, so E[a a] = sum kx^4 / |k|^2 H etc. (H = |h0(k)|^2 + |h0(-k)|^2); and of their time derivatives,
// the same sums weighted by omega^2 = g |k| (deep water). Signed sums as two's-complement 64-bit fixed point.
// Accumulator layout (uint64 each): 0..39 per-level sigma_J^2; 40..87 Sigma (aa, bb, cc, ab, ac, bc); 88..135 the
// derivatives' Sigma in the same order; 136 J_t (float, FoamCalibrate).
// P[0] h0 SRV (raw, float4 per bin: h0(k), conj(h0(-k)); Ocean.hlsli), accumulator UAV (raw), levels, 0; P[1] s_0 (m);
// P[2] cascade lengths (Ocean.hlsli oceanLength)
#include "Ocean.hlsli"

void foamAddSigned(RWByteAddressBuffer accumulator, uint offset, float value)
{
    const float sum = WaveActiveSum(value);
    if (WaveIsFirstLane() && sum != 0) accumulator.InterlockedAdd64(offset, uint64_t(int64_t(round(sum * 1099511627776.0))));
}

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    float w = 0, k = 0, H = 0, A = 0, B = 0, C = 0;
    if (i < OCEAN_N * OCEAN_N * 3)
    {
        const uint cascade = i / (OCEAN_N * OCEAN_N), bin = i % (OCEAN_N * OCEAN_N);
        ByteAddressBuffer h0 = ResourceDescriptorHeap[P[0].x];
        const float4 h = asfloat(h0.Load4(16 * i));
        const float2 kv = oceanK(uint2(bin % OCEAN_N, bin / OCEAN_N), oceanLength(cascade));
        k = length(kv);
        H = dot(h.xy, h.xy) + dot(h.zw, h.zw);
        w = dot(kv, kv) * H;
        if (k > 0)
        {
            A = kv.x * kv.x / k;
            B = kv.y * kv.y / k;
            C = kv.x * kv.y / k;
        }
    }
    RWByteAddressBuffer accumulator = ResourceDescriptorHeap[P[0].y];
    const float s0 = asfloat(P[1].x);
    for (uint level = 0; level < P[0].z; ++level)
    {
        const float cut = OCEAN_PI / (s0 * float(1u << (2 * level)));
        const float sum = WaveActiveSum(k > cut ? w : 0.0);
        if (WaveIsFirstLane() && sum > 0) accumulator.InterlockedAdd64(8 * level, uint64_t(sum * 1099511627776.0 + 0.5));
    }
    const float terms[6] = { A * A * H, B * B * H, C * C * H, A * B * H, A * C * H, B * C * H };
    const float omega2 = OCEAN_G * k;
    [unroll] for (uint t = 0; t < 6; ++t)
    {
        foamAddSigned(accumulator, 40 + 8 * t, terms[t]);
        foamAddSigned(accumulator, 88 + 8 * t, omega2 * terms[t]);
    }
}
