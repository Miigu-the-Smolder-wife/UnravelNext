// unx-kernel: cs_6_6 main
// Reconstruct each mirrored bin directly in the inverse FFT's shared tile.
// The persistent 257^2 modes are unchanged; no 512^2 intermediate spectrum
// needs to be written by PoolEvolve and read back by this pass.
#include "Pool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint z : SV_GroupID)
{
    ByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        const uint2 mode = uint2(poolFold(x), poolFold(z));
        const float2 hp = asfloat(modes.Load2(8 * (mode.y * POOL_Q + mode.x)));
        const float2 k = float2(mode) * (OCEAN_PI / (float(POOL_N / 2u) * poolH()));
        const float2 ks = select(mode == POOL_N / 2u, float2(0, 0), k);
        const float kx = x > POOL_N / 2u ? -ks.x : ks.x;
        const float kz = z > POOL_N / 2u ? -ks.y : ks.y;
        const uint r = bitReverse9(x);
        g_fft[0][r] = hp;
        g_fft[1][r] = float2(-kz * hp.x, kx * hp.x);
    }
    fftInverse(t);
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        spectrum.Store4(16 * (z * POOL_PITCH + x), asuint(float4(g_fft[0][x], g_fft[1][x])));
    }
}
