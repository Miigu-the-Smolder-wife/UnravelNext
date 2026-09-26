// unx-kernel: cs_6_6 main
// Ocean, per frame, pass 2 of 2: one group per (cascade, column x): the inverse FFT along z of the row results, the
// centring sign (-1)^(x + z) (the spectrum is stored centred, Ocean.hlsli oceanK), and the fields at texel (x, z):
//   slice 2c:     (Dx, h, Dz, J)               J = (1 + dDx/dx)(1 + dDz/dz) - (dDx/dz)^2 (foam where J < 1)
//   slice 2c + 1: (dh/dx, dh/dz, dDx/dx, dDz/dz)
// in metres over the texel's rest position (x, z) L / N of the periodic tile.
// Root constants: as OceanRows.hlsl
#include "OceanFft.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    uint cascade = g / OCEAN_N, x = g % OCEAN_N;
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        uint z = t + part * 256u, at = 32 * ((cascade * OCEAN_N + z) * OCEAN_PITCH + x);
        float4 a = asfloat(spectrum.Load4(at)), b = asfloat(spectrum.Load4(at + 16));
        uint r = bitReverse9(z);
        g_fft[0][r] = a.xy;
        g_fft[1][r] = a.zw;
        g_fft[2][r] = b.xy;
        g_fft[3][r] = b.zw;
    }
    fftInverse(t);
    RWTexture2DArray<float4> field = ResourceDescriptorHeap[P[0].z];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        uint z = t + part2 * 256u;
        float sgn = ((x + z) & 1u) ? -1.0 : 1.0;
        float2 c0 = g_fft[0][z] * sgn, c1 = g_fft[1][z] * sgn, c2 = g_fft[2][z] * sgn, c3 = g_fft[3][z] * sgn;
        float jacobian = (1.0 + c2.y) * (1.0 + c3.x) - c3.y * c3.y;
        field[uint3(x, z, 2 * cascade)] = float4(c0.x, c0.y, c1.x, jacobian);
        field[uint3(x, z, 2 * cascade + 1)] = float4(c1.y, c2.x, c2.y, c3.x);
    }
}
