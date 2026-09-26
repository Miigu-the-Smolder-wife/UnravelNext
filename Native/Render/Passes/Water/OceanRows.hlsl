// unx-kernel: cs_6_6 main
// Ocean, per frame, pass 1 of 2: one group per (cascade, row z): the packed spectra of h(k, t) for the row (Ocean.hlsli
// packing), then the inverse FFT along x. The row results go to the spectrum buffer in row order (bin (x, z) of cascade
// c at 32 (c N P + z P + x), pitch P = N + 1, Ocean.hlsli): coalesced stores; the column pass reads 32 B sectors down a
// column.
// Root constants: P[0] h0 SRV (raw), spectrum UAV (raw), displacement UAV (columns), cascade count; P[1] u = frac(t / T)
// 2^32 (Ocean.hlsli), frequency index SRV (raw, uint m per bin), twiddle SRV (raw, float2 x N/2), slopes UAV (columns); P[2] cascade lengths (m) 0..2
#include "OceanFft.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    uint cascade = g / OCEAN_N, z = g % OCEAN_N;
    float L = oceanLength(cascade);
    ByteAddressBuffer h0 = ResourceDescriptorHeap[P[0].x], frequencies = ResourceDescriptorHeap[P[1].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        uint x = t + part * 256u;
        uint bin = cascade * OCEAN_N * OCEAN_N + z * OCEAN_N + x;
        float4 pair = asfloat(h0.Load4(16 * bin));
        float2 k = oceanK(uint2(x, z), L);
        float kLen = length(k);
        float2 e = oceanPhase(frequencies.Load(4 * bin), P[1].x);
        float2 h = cmul(pair.xy, e) + cmul(pair.zw, float2(e.x, -e.y));  // h0(k) e^{-iwt} + conj(h0(-k)) e^{iwt}
        float2 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        if (kLen > 0)
        {
            float2 ih = float2(-h.y, h.x);                                     // i h
            float2 dx = (k.x / kLen) * ih, dz = (k.y / kLen) * ih;             // D = i k / |k| h
            float2 dhdx = k.x * ih, dhdz = k.y * ih;                           // grad h = i k h
            float2 dDxdx = -(k.x * k.x / kLen) * h, dDzdz = -(k.y * k.y / kLen) * h, dDxdz = -(k.x * k.y / kLen) * h;  // i k D
            c0 = dx + ih;                                                      // Dx + i h
            c1 = dz + float2(-dhdx.y, dhdx.x);                                 // Dz + i dh/dx
            c2 = dhdz + float2(-dDxdx.y, dDxdx.x);                             // dh/dz + i dDx/dx
            c3 = dDzdz + float2(-dDxdz.y, dDxdz.x);                            // dDz/dz + i dDx/dz
        }
        uint r = bitReverse9(x);
        g_fft[0][r] = c0;
        g_fft[1][r] = c1;
        g_fft[2][r] = c2;
        g_fft[3][r] = c3;
    }
    fftInverse(t);
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        uint x = t + part2 * 256u, at = 32 * ((cascade * OCEAN_N + z) * OCEAN_PITCH + x);
        spectrum.Store4(at, asuint(float4(g_fft[0][x], g_fft[1][x])));
        spectrum.Store4(at + 16, asuint(float4(g_fft[2][x], g_fft[3][x])));
    }
}
