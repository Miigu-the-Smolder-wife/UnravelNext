// unx-kernel: cs_6_6 main
// Round basins, per evolution, last pass: one group per ring j: the inverse angular FFT (unnormalised: RoundRows put
// 1 / N in the forward) of four channels - eta (bins m and N - m = conj), phi, the angular slope (1 / r) d eta / d theta
// (bin m x i m / r_j), the radial slope (RoundSynthesis) - then the output texel (eta, eta_r, eta_theta / r, phi), the
// previous eta (the stream's motion), and the accumulation cleared.
#include "Passes/Water/RoundPool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint j : SV_GroupID)
{
    ByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].z];
    const float invR = 1.0 / roundRingRadius(j);
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint bin = t + part * 256u;  // 0 .. 511
        float4 hp = 0;
        float2 dr = 0, dtheta = 0;
        if (bin < ROUND_ORDERS)
        {
            const uint at = 48 * (j * ROUND_ORDERS + bin);
            hp = asfloat(spectrum.Load4(at));
            dr = asfloat(spectrum.Load2(at + 16));
            dtheta = float2(-hp.y, hp.x) * (float(bin) * invR);  // i m H_m / r
        }
        else if (bin > ROUND_ORDERS)
        {
            const uint m = ROUND_THETA - bin;  // negative order -m: conjugates (the field is real)
            const uint at = 48 * (j * ROUND_ORDERS + m);
            const float4 v = asfloat(spectrum.Load4(at));
            hp = float4(v.x, -v.y, v.z, -v.w);
            const float2 d = asfloat(spectrum.Load2(at + 16));
            dr = float2(d.x, -d.y);
            dtheta = float2(hp.y, -hp.x) * (float(m) * invR);  // i (-m) conj(H_m) / r
        }
        const uint r = bitReverse9(bin);
        g_fft[0][r] = hp.xy;
        g_fft[1][r] = hp.zw;
        g_fft[2][r] = dr;
        g_fft[3][r] = dtheta;
    }
    fftInverse(t);
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer previous = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[4].x];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint i = t + part2 * 256u;
        const uint at = i + j * ROUND_THETA;
        previous.Store(4 * at, asuint(output[uint2(i, j)].x));
        accum.Store2(8 * at, uint2(0, 0));
        output[uint2(i, j)] = float4(g_fft[0][i].x, g_fft[2][i].x, g_fft[3][i].x, g_fft[1][i].x);
    }
}
