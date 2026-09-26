// Ocean surface spectrum and FFT (track W, B7; FEATURES_GAME 1, ARCHITECTURE 2.14 "water FFT 3 cascades 512^2"):
// a deep-water wave field h(x, t) = sum_k h(k, t) e^{i k.x} on three periodic cascades of N = 512, each covering its own
// band of wavenumbers (no double counting), with the choppy horizontal displacement D = i k/|k| h (lambda 1: the
// deep-water surface particle orbit of linear theory, x' = x - (k/|k|) A sin(k.x - w t) for h = A cos(k.x - w t); the
// sign sharpens crests, not troughs), the slopes and the Jacobian terms for foam (FEATURES_GAME 1.3 (f)).
//   Spectrum: JONSWAP (fetch-limited: alpha = 0.076 (U^2 / (F g))^0.22, omega_p = 22 (g^2 / (U F))^(1/3), gamma = 3.3)
//             with a cos^(2s)(theta/2) directional spreading about the wind (s = spread), converted to wavenumber space:
//             E(kx, kz) = S(omega) D(theta) (d omega / d k) / k, amplitude variance per bin E dk^2 (dk = 2 pi / L).
//   h0(k) = (xi_r + i xi_i) sqrt(E dk^2 / 2), xi standard normal from a hash of (seed, cascade, index): deterministic.
//   h(k, t) = h0(k) e^{-i w t} + conj(h0(-k)) e^{i w t}, w = sqrt(g |k|) (a real field; the h0(k) term travels along +k,
//   so the spectrum's wind direction is the direction the waves travel).
//   Time: the dispersion is quantised to w_m = m w_0, m = round(w / w_0), w_0 = 2 pi / T (T = 4096 s, Tessendorf's
//   repeat period; |w_m - w| <= 7.7e-4 rad/s, 0.3% at the slowest bin of a 1 km tile), so the phase is
//   2 pi frac(m t / T), computed exactly in 32-bit integers from u = frac(t / T) 2^32 (the host's double): the phase
//   error stays ~1e-6 rad at any game time, where float(w t) loses a radian after hours. m per bin comes from the host
//   (Ocean.cpp, double precision, uploaded with the tile lengths), the single definition the tests also use.
// Packing: four complex IFFTs per cascade, each carrying two real fields as A + iB:
//   0: Dx + i h, 1: Dz + i dh/dx, 2: dh/dz + i dDx/dx, 3: dDz/dz + i dDx/dz.
//   The Nyquist row and column (index 0 on either axis) are zero: -k is not representable there, so the packed
//   derivative spectra would not be Hermitian (the derivative of the Nyquist mode is undefined on the grid).
// Root constants: OceanSpectrum.hlsl and OceanRows/Columns.hlsl list theirs; both put the cascade lengths (m) in P[2].
#ifndef UNX_WATER_OCEAN_HLSLI
#define UNX_WATER_OCEAN_HLSLI
#include "Bindless.hlsli"

#define OCEAN_N 512u
#define OCEAN_LOG2N 9u
#define OCEAN_G 9.81
#define OCEAN_PI 3.14159265358979
#define OCEAN_PITCH (OCEAN_N + 1u)  // intermediate row pitch in elements: a power-of-two pitch puts a column's accesses on one
                                   // memory partition (measured 0.24 ms for the column pass against 0.05 ms coalesced)

float oceanLength(uint cascade) { return asfloat(cascade == 0 ? P[2].x : (cascade == 1 ? P[2].y : P[2].z)); }
// Wavenumber of array index (m, n): k = 2 pi (m - N/2, n - N/2) / L (the spectrum is stored centred).
float2 oceanK(uint2 index, float L) { return (float2(index) - OCEAN_N / 2.0) * (2.0 * OCEAN_PI / L); }
float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 cexpi(float phase) { float s, c; sincos(phase, s, c); return float2(c, s); }
// e^{-i w_m t} for the bin's frequency index m at the frame's u = frac(t / T) 2^32.
float2 oceanPhase(uint m, uint u) { return cexpi(-2.0 * OCEAN_PI * (float(m * u) * (1.0 / 4294967296.0))); }
// Amplitude variance E(k) dk^2 of the bin at wavevector k, or 0 outside the cascade's band (spectrum kernel constants:
// P[1] U, F, wind unit vector xz; P[3] band boundaries k1, k2, spreading normalisation, spread s). The spreading factor
// cos^(2s)(theta / 2) = ((1 + cos theta) / 2)^s = (|k^ + w^|^2 / 4)^s uses no angle: no transcendental error for pow to
// amplify (s = 8 raises cos to the 16th).
float oceanVariance(float2 k, float L, uint cascade)
{
    float kLen = length(k);
    float kLow = cascade == 0 ? 0.0 : asfloat(cascade == 1 ? P[3].x : P[3].y);
    float kHigh = cascade == 2 ? 1e30 : asfloat(cascade == 0 ? P[3].x : P[3].y);
    if (!(kLen > 0) || kLen < kLow || kLen >= kHigh) return 0;
    float U = asfloat(P[1].x), F = asfloat(P[1].y), spread = asfloat(P[3].w);
    float2 half2 = k / kLen + asfloat(P[1].zw);
    float g = OCEAN_G, omega = sqrt(g * kLen);
    float alpha = 0.076 * pow(U * U / (F * g), 0.22), omegaP = 22.0 * pow(g * g / (U * F), 1.0 / 3.0);
    float sigma = omega <= omegaP ? 0.07 : 0.09, d = (omega - omegaP) / (sigma * omegaP);
    float S = alpha * g * g / pow(omega, 5.0) * exp(-1.25 * pow(omegaP / omega, 4.0)) * pow(3.3, exp(-0.5 * d * d));
    float D = asfloat(P[3].z) * pow(0.25 * dot(half2, half2), spread);
    float dk = 2.0 * OCEAN_PI / L;
    return S * D * (g / (2.0 * omega)) / kLen * dk * dk;
}
#endif
