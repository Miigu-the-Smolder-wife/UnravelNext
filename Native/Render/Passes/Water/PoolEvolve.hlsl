// unx-kernel: cs_6_6 main
// Closed basins, per evolution: one thread per basin mode (m, n), m, n = 0..256. The state is spectral: the mode's height
// and potential amplitudes (H, Phi), real (the mirrored field is real and even), so no float round trip feeds back into
// it: each frame rotates it exactly (Ripple's rotation, normalised) and damps it by exp(-delta dt) (Pool.hlsli), and only
// frames with sources add their forward transform (P[4].w bit 0: add the spectrum's bin (m, n), bit 1: replace the state
// by it - setState). k = (m pi / Lx, n pi / Lz): the mode cos(m pi x / Lx) cos(n pi z / Lz), bin (m, n) of the mirrored
// 512 x 512 domain and its images (+-m, +-n). The thread then writes those four bins for the inverse passes: (eta + i phi)
// and the slope spectrum i kx H - kz H (eta_x + i eta_z) with the bin's signed wavenumbers; along a Nyquist axis
// (m = 256: cos(pi i)) the derivative is zero at every sample, so that slope term is zero. k = 0 keeps the mean height;
// the mean potential has no restoring force and is dropped.
#include "Pool.hlsli"

float2 ctimesI(float2 a) { return float2(-a.y, a.x); }

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= POOL_Q * POOL_Q) return;
    const uint2 mode = uint2(i % POOL_Q, i / POOL_Q);
    RWByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    const uint flags = P[4].w;
    float2 hp = asfloat(modes.Load2(8 * i));  // (H, Phi)
    if (flags & 3u)
    {
        const float2 added = asfloat(spectrum.Load2(16 * (mode.y * POOL_PITCH + mode.x)));  // (H, Phi) of the increment
        hp = (flags & 2u) ? added : hp + added;
    }
    float H = hp.x, Phi = hp.y;
    const float2 h = poolH();
    const float2 k = float2(mode) * (OCEAN_PI / (float(POOL_N / 2u) * h));
    ByteAddressBuffer table = ResourceDescriptorHeap[P[5].y];
    const float4 e = asfloat(table.Load4(16 * i));  // w, K / w, w / K, delta: the host's double values (Pool.cpp modeTable)
    if (i != 0)
    {
        const float dt = poolDt();
        float s, c;
        poolSinCos(e.x * dt, s, c);
        const float unit = rsqrt(c * c + s * s);  // a rotation: exactly of length 1, so the gain is the damping alone
        c *= unit;
        s *= unit;
        const float decay = exp(-e.w * dt);
        const float h2 = (H * c + Phi * (e.y * s)) * decay, p2 = (Phi * c - H * (e.z * s)) * decay;
        H = h2;
        Phi = p2;
    }
    else Phi = 0;
    modes.Store2(8 * i, asuint(float2(H, Phi)));
    const float2 ks = select(mode == POOL_N / 2u, float2(0, 0), k);  // slope wavenumbers (Nyquist: zero derivative)
    [unroll] for (uint image = 0; image < 4; ++image)
    {
        const bool negX = (image & 1u) != 0, negZ = (image & 2u) != 0;
        if ((negX && (mode.x == 0 || mode.x == POOL_N / 2u)) || (negZ && (mode.y == 0 || mode.y == POOL_N / 2u))) continue;  // its own image
        const uint2 bin = uint2(negX ? POOL_N - mode.x : mode.x, negZ ? POOL_N - mode.y : mode.y);
        const float kx = negX ? -ks.x : ks.x, kz = negZ ? -ks.y : ks.y;
        const float2 slope = float2(-kz * H, kx * H);  // i kx H - kz H, H real
        spectrum.Store4(16 * (bin.y * POOL_PITCH + bin.x), asuint(float4(H, Phi, slope)));
    }
}
