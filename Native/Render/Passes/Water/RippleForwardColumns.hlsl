// unx-kernel: cs_6_6 main
// Ripples, per frame, pass 2: one group per column x: the forward FFT along z (conj(IFFT(conj(.)))), normalised by
// 1 / N^2: Z(k) = (1 / N^2) sum_x (eta + i phi)(x) e^{-i k.x}, so the inverse passes' unnormalised sums rebuild the field.
#include "Ripple.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint x : SV_GroupID)
{
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint z = t + part * 256u;
        const float2 v = asfloat(spectrum.Load2(16 * (z * RIPPLE_PITCH + x)));
        const uint r = bitReverse9(z);
        g_fft[0][r] = v;  // the row pass stored conj(A), A = the forward row transform: IFFT(conj(A)) conjugated = forward
        g_fft[1][r] = 0;
    }
    fftInverse(t);
    const float norm = 1.0 / float(RIPPLE_N * RIPPLE_N);
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint z = t + part2 * 256u;
        const float2 c = g_fft[0][z];
        spectrum.Store2(16 * (z * RIPPLE_PITCH + x), asuint(float2(c.x, -c.y) * norm));
    }
}
