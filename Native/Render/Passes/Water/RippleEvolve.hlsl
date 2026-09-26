// unx-kernel: cs_6_6 main
// Ripples, per frame, pass 3: one thread per wavenumber pair (k, -k). The packed spectrum Z = H + i Phi (H, Phi the
// Hermitian spectra of eta and phi) is split with its mirror, each mode is rotated exactly over dt (Ripple.hlsli) and
// decays by exp(-2 nu k^2 dt); the slope spectrum S = i k_x H + i (i k_z H) (eta_x + i eta_z) is written beside it.
// k = 0 keeps the mean height and drops the mean potential (no restoring force acts on it); Nyquist bins are zero.
#include "Ripple.hlsli"

float2 cconj(float2 a) { return float2(a.x, -a.y); }
float2 ctimesI(float2 a) { return float2(-a.y, a.x); }

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= RIPPLE_N * RIPPLE_N) return;
    const uint2 index = uint2(i % RIPPLE_N, i / RIPPLE_N);
    const uint2 mirror = uint2((RIPPLE_N - index.x) % RIPPLE_N, (RIPPLE_N - index.y) % RIPPLE_N);
    const uint self = index.y * RIPPLE_N + index.x, other = mirror.y * RIPPLE_N + mirror.x;
    if (other < self) return;  // the pair is handled by its lower index
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].y];
    const uint atK = 16 * (index.y * RIPPLE_PITCH + index.x), atM = 16 * (mirror.y * RIPPLE_PITCH + mirror.x);
    if (rippleNyquist(index))
    {
        spectrum.Store4(atK, uint4(0, 0, 0, 0));
        if (other != self) spectrum.Store4(atM, uint4(0, 0, 0, 0));
        return;
    }
    const float2 zk = asfloat(spectrum.Load2(atK)), zm = asfloat(spectrum.Load2(atM));
    float2 H = 0.5 * (zk + cconj(zm)), Phi = 0.5 * ctimesI(cconj(zm) - zk);  // (zk - conj(zm)) / (2i)
    const float2 k = rippleK(index);
    const float kLen = length(k);
    if (kLen > 0)
    {
        const float d = rippleDepth(), K = d > 0 ? kLen * tanh(kLen * d) : kLen;
        const float G = asfloat(P[3].x) + asfloat(P[3].y) * kLen * kLen, w = sqrt(K * G), dt = rippleDt();
        float s, c;
        sincos(w * dt, s, c);
        const float decay = exp(-2.0 * asfloat(P[3].z) * kLen * kLen * dt);
        const float2 h2 = (H * c + Phi * (K / w * s)) * decay, p2 = (Phi * c - H * (w / K * s)) * decay;
        H = h2;
        Phi = p2;
    }
    else Phi = 0;
    const float2 slope = ctimesI(H * k.x) - H * k.y;  // i kx H + i (i kz H)
    spectrum.Store4(atK, asuint(float4(H + ctimesI(Phi), slope)));
    if (other != self)
    {
        const float2 Hm = cconj(H), Pm = cconj(Phi);
        const float2 slopeM = ctimesI(Hm * -k.x) - Hm * -k.y;
        spectrum.Store4(atM, asuint(float4(Hm + ctimesI(Pm), slopeM)));
    }
}
