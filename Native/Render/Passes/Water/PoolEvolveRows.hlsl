// unx-kernel: cs_6_6 main
// One group owns one canonical basin row, including its mirrored image. Evolve
// each persistent mode once, then transform it without publishing a full 512^2
// spectral intermediate. The two z images share the height/potential transform;
// only the z derivative changes sign. No inter-group rendezvous is required.
#include "Pool.hlsli"

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint n : SV_GroupID)
{
    RWByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer table = ResourceDescriptorHeap[P[5].y];
    const uint flags = P[4].w;
    const float2 waveNumber = OCEAN_PI / (float(POOL_N / 2u) * poolH());
    for (uint m = t; m < POOL_Q; m += 256u)
    {
        const uint i = n * POOL_Q + m;
        float2 hp = asfloat(modes.Load2(8 * i));
        if (flags & 3u)
        {
            const float2 added = asfloat(spectrum.Load2(16 * (n * POOL_PITCH + m)));
            hp = (flags & 2u) ? added : hp + added;
        }
        const float4 e = asfloat(table.Load4(16 * i));
        if (i != 0)
        {
            float sine, cosine;
            poolSinCos(e.x * poolDt(), sine, cosine);
            const float unit = rsqrt(cosine * cosine + sine * sine);
            cosine *= unit;
            sine *= unit;
            const float decay = exp(-e.w * poolDt());
            hp = float2(hp.x * cosine + hp.y * (e.y * sine),
                        hp.y * cosine - hp.x * (e.z * sine)) * decay;
        }
        else hp.y = 0;
        modes.Store2(8 * i, asuint(hp));
        const float kx = m == POOL_N / 2u ? 0.0 : float(m) * waveNumber.x;
        g_fft[0][bitReverse9(m)] = hp;
        g_fft[1][bitReverse9(m)] = float2(0, kx * hp.x);
        if (m != 0 && m != POOL_N / 2u)
        {
            g_fft[0][bitReverse9(POOL_N - m)] = hp;
            g_fft[1][bitReverse9(POOL_N - m)] = float2(0, -kx * hp.x);
        }
    }
    // fftInverse starts with a group barrier: every mode and mirrored input is
    // ready before the first butterfly; stores below follow its final barrier.
    fftInverse(t);
    const float kz = n == POOL_N / 2u ? 0.0 : float(n) * waveNumber.y;
    // The column transform only consumes basin columns, including both walls.
    for (uint x = t; x < POOL_Q; x += 256u)
    {
        const float2 field = g_fft[0][x], dx = g_fft[1][x];
        spectrum.Store4(16 * (n * POOL_PITCH + x), asuint(float4(field, dx.x - kz * field.x, dx.y)));
        if (n != 0 && n != POOL_N / 2u)
            spectrum.Store4(16 * ((POOL_N - n) * POOL_PITCH + x), asuint(float4(field, dx.x + kz * field.x, dx.y)));
    }
}
