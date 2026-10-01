// unx-kernel: cs_6_6 main
// Round basins, frames with sources: one group per ring j: the increment at its 512 angles (the accumulated sources plus
// the mean-level term P[4].w) and the forward angular FFT of eta (channel 0) and phi (channel 1) as conj(IFFT(conj(.))),
// normalised by 1 / N_theta: H_m(j) = (1 / N) sum_i eta(theta_i, r_j) e^{-i m theta_i}, orders m = 0 .. 255 stored
// (the negative orders follow by conjugation: eta is real). The accumulation is cleared by RoundColumns.
#include "Passes/Water/RoundPool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint j : SV_GroupID)
{
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    const float meanShift = asfloat(P[4].w);
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint i = t + part * 256u;
        const int2 added = asint(accum.Load2(8 * (i + j * ROUND_THETA)));
        const float2 v = float2(added) * (1.0 / ROUND_FIXED) + float2(meanShift, 0);
        const uint r = bitReverse9(i);
        g_fft[0][r] = float2(v.x, 0);   // conj(eta): eta real
        g_fft[1][r] = float2(v.y, 0);   // conj(phi)
        g_fft[2][r] = 0;
        g_fft[3][r] = 0;
    }
    fftInverse(t);
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].z];
    const float norm = 1.0 / float(ROUND_THETA);
    if (t < ROUND_ORDERS)
    {
        // IFFT(conj(x)) conjugated = the forward transform: bin m
        const float2 H = float2(g_fft[0][t].x, -g_fft[0][t].y) * norm, Phi = float2(g_fft[1][t].x, -g_fft[1][t].y) * norm;
        spectrum.Store4(48 * (j * ROUND_ORDERS + t), asuint(float4(H, Phi)));
    }
}
