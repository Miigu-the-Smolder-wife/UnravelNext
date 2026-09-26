// unx-kernel: cs_6_6 main
// Ripples, per frame, pass 1: one group per row z of the (moved) window: the state at the old window's texel (x + shift)
// (zero where the window uncovered new water), plus the accumulated sources (then cleared), times the sponge; the
// forward FFT along x as conj(IFFT(conj(.))) (the column pass conjugates back and normalises).
#include "Ripple.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint z : SV_GroupID)
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    const int2 shift = int2(asint(P[2].z), asint(P[2].w));
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        const int2 old = int2(x, z) + shift;
        float2 v = 0;
        if (all(old >= 0) && all(old < int(RIPPLE_N))) v = asfloat(state.Load2(8 * (old.y * RIPPLE_N + old.x)));
        const uint at = 8 * (z * RIPPLE_N + x);
        const int2 added = asint(accum.Load2(at));
        if (any(added != 0)) accum.Store2(at, uint2(0, 0));
        v = (v + float2(added) * (1.0 / RIPPLE_FIXED)) * rippleSponge(uint2(x, z));
        const uint r = bitReverse9(x);
        g_fft[0][r] = float2(v.x, -v.y);  // conj(eta + i phi)
        g_fft[1][r] = 0;
    }
    fftInverse(t);
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    [unroll] for (uint part2 = 0; part2 < 2; ++part2)
    {
        const uint x = t + part2 * 256u;
        spectrum.Store2(16 * (z * RIPPLE_PITCH + x), asuint(g_fft[0][x]));
    }
}
