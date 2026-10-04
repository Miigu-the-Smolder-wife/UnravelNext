// unx-kernel: cs_6_6 main
// Closed basins, frames with sources (or setState): one group per canonical row z (0..256): the increment
// at the folded position (x, z) (Pool.hlsli: the even mirror across both walls) - the accumulated sources plus their
// mean-level term (P[3].w), plus the uploaded field when it replaces the state (P[4].w bit 1) - and the forward FFT
// along x as conj(IFFT(conj(.))) (RippleForwardColumns conjugates back and normalises; PoolEvolve adds the result).
// The accumulation is cleared by PoolColumns (every basin sample is read here by up to four mirrored positions).
#include "Pool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint z : SV_GroupID)
{
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer input = ResourceDescriptorHeap[P[5].x];
    const float meanShift = asfloat(P[3].w);
    const bool replace = (P[4].w & 2u) != 0;
    const uint qz = poolFold(z);
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        const uint at = qz * POOL_Q + poolFold(x);
        float2 v = replace ? asfloat(input.Load2(8 * at)) : float2(0, 0);
        const int2 added = asint(accum.Load2(8 * at));
        v += float2(added) * (1.0 / POOL_FIXED) + float2(meanShift, 0);
        g_fft[0][bitReverse9(x)] = float2(v.x, -v.y);  // conj(eta + i phi)
        g_fft[1][bitReverse9(x)] = 0;
    }
    fftInverse(t);
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint x = t + part2 * 256u;
        spectrum.Store2(16 * (z * POOL_PITCH + x), asuint(g_fft[0][x]));
        // An even wall extension has identical rows at z and N-z. Publish both
        // from the same FFT instead of dispatching the mirrored row again.
        if (z != 0 && z != POOL_N / 2u)
            spectrum.Store2(16 * ((POOL_N - z) * POOL_PITCH + x), asuint(g_fft[0][x]));
    }
}
