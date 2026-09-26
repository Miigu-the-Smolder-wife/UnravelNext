// Radix-2 inverse FFT of N = 512 complex values in four packed channels (Ocean.hlsli), 256 threads, in groupshared
// memory: the caller stores the input in bit-reversed order, then log2 N butterfly stages with twiddles e^{+2 pi i j / m}
// (unnormalised: the spectrum amplitudes are the field's Fourier coefficients). The twiddles e^{2 pi i j / N}, j < N/2,
// come from the host in double precision rounded to float (P[1].z): hardware sin/cos would add their absolute error at
// every stage.
#ifndef UNX_WATER_OCEAN_FFT_HLSLI
#define UNX_WATER_OCEAN_FFT_HLSLI
#include "Ocean.hlsli"

#ifndef FFT_CHANNELS
#define FFT_CHANNELS 4u  // complex channels transformed together (Ocean: 4; Ripple: 2)
#endif
groupshared float2 g_fft[FFT_CHANNELS][OCEAN_N];
groupshared float2 g_twiddle[OCEAN_N / 2];
uint bitReverse9(uint v) { return reversebits(v) >> (32u - OCEAN_LOG2N); }
void fftInverse(uint t)
{
    ByteAddressBuffer twiddles = ResourceDescriptorHeap[P[1].z];
    g_twiddle[t] = asfloat(twiddles.Load2(8 * t));
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stage = 1; stage <= OCEAN_LOG2N; ++stage)
    {
        uint halfSpan = 1u << (stage - 1), span = halfSpan << 1;
        uint pos = t % halfSpan, i0 = (t / halfSpan) * span + pos, i1 = i0 + halfSpan;
        float2 w = g_twiddle[pos << (OCEAN_LOG2N - stage)];  // e^{2 pi i pos / span}
        [unroll] for (uint c = 0; c < FFT_CHANNELS; ++c)
        {
            float2 a = g_fft[c][i0], b = cmul(w, g_fft[c][i1]);
            g_fft[c][i0] = a + b;
            g_fft[c][i1] = a - b;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
#endif
