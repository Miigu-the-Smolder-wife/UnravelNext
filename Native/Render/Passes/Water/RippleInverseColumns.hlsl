// unx-kernel: cs_6_6 main
// Ripples, per frame, pass 5: one group per column x: the inverse FFT along z; the state (eta, phi) for the next frame
// and the output texel (eta, eta_x, eta_z, phi) at (x, z) of the window.
#include "Ripple.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint x : SV_GroupID)
{
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint z = t + part * 256u;
        const float4 v = asfloat(spectrum.Load4(16 * (z * RIPPLE_PITCH + x)));
        const uint r = bitReverse9(z);
        g_fft[0][r] = v.xy;
        g_fft[1][r] = v.zw;
    }
    fftInverse(t);
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint z = t + part2 * 256u;
        const float2 a = g_fft[0][z], b = g_fft[1][z];
        state.Store2(8 * (z * RIPPLE_N + x), asuint(a));
        output[uint2(x, z)] = float4(a.x, b.x, b.y, a.y);
    }
}
