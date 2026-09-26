// unx-kernel: cs_6_6 main
// Ripples, per frame, pass 4: one group per row z: the inverse FFT along x of both channels (eta + i phi, eta_x + i eta_z).
#include "Ripple.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint z : SV_GroupID)
{
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        const float4 v = asfloat(spectrum.Load4(16 * (z * RIPPLE_PITCH + x)));
        const uint r = bitReverse9(x);
        g_fft[0][r] = v.xy;
        g_fft[1][r] = v.zw;
    }
    fftInverse(t);
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint x = t + part2 * 256u;
        spectrum.Store4(16 * (z * RIPPLE_PITCH + x), asuint(float4(g_fft[0][x], g_fft[1][x])));
    }
}
