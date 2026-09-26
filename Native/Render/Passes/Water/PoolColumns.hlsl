// unx-kernel: cs_6_6 main
// Closed basins, per evolution, last pass: one group per basin column x (0..256): the inverse FFT along z of both channels
// (after RippleInverseRows); the basin samples z = 0..256: the previous eta (the output before this evolution: the
// triangle stream's motion), the output texel (eta, eta_x, eta_z, phi), the source accumulation cleared.
#include "Pool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint x : SV_GroupID)
{
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint z = t + part * 256u;
        const float4 v = asfloat(spectrum.Load4(16 * (z * POOL_PITCH + x)));
        const uint r = bitReverse9(z);
        g_fft[0][r] = v.xy;
        g_fft[1][r] = v.zw;
    }
    fftInverse(t);
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer previous = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    for (uint z = t; z < POOL_Q; z += 256u)
    {
        const float2 a = g_fft[0][z], b = g_fft[1][z];
        const uint at = z * POOL_Q + x;
        previous.Store(4 * at, asuint(output[uint2(x, z)].x));
        accum.Store2(8 * at, uint2(0, 0));
        output[uint2(x, z)] = float4(a.x, b.x, b.y, a.y);
    }
}
