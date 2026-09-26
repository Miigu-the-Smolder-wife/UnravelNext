// Local ripples W (track W, B7; FEATURES_GAME 1.3 (f)): the linear water surface around the focus (player), 512^2
// texels of h = 5 cm (25.6 m), evolved every frame in the spectral domain with the exact linear dispersion of
// gravity-capillary waves over depth d,
//   w(k)^2 = K(k) G(k),  K = k tanh(k d) (deep water: k),  G = g + (sigma / rho) k^2,
// and the viscous decay exp(-2 nu k^2 t) of each mode: no time-step or grid-dispersion error for any resolved wavelength
// (a finite-difference wave equation has neither the right phase speeds nor capillarity).
// State (space domain): surface height eta (m) and surface velocity potential phi (m^2/s). Per mode:
//   eta_t = K phi,  phi_t = -G eta   =>   exact rotation over dt:
//   eta' = eta cos(w dt) + (K / w) phi sin(w dt),  phi' = phi cos(w dt) - (w / K) eta sin(w dt).
// Sources are impulsive surface pressures: a vertical impulse I (N s) spread over a Gaussian footprint changes phi by
// -(I / rho) g(x), sum g h^2 = 1 over the footprint (a Gaussian sampled out to 4 sigma and normalised by its own
// discrete sum: the water's vertical momentum rho sum phi h^2 changes by exactly -I). They are accumulated in 32-bit
// fixed point (order independent) and consumed by the next frame.
// Open boundary: an absorbing sponge (the outer 48 texels) damps both fields each frame, so waves leave the window
// instead of wrapping around the periodic domain. The window follows the focus in whole texels (RippleShift).
// Layout: the spectrum is stored in natural order (index m <-> wavenumber (m < N/2 ? m : m - N) 2 pi / (N h)), the
// Nyquist row and column are zero. The FFT is OceanFft.hlsli with 2 channels: (eta + i phi), (eta_x + i eta_z).
#ifndef UNX_WATER_RIPPLE_HLSLI
#define UNX_WATER_RIPPLE_HLSLI
#define FFT_CHANNELS 2u
#include "OceanFft.hlsli"

#define RIPPLE_N OCEAN_N
#define RIPPLE_PITCH OCEAN_PITCH
#define RIPPLE_SPONGE 48u
#define RIPPLE_FIXED 16777216.0  // source accumulation: 2^24 per unit (eta m, phi m^2/s)
// Root constants shared by the ripple kernels:
//   P[0] state UAV (raw, float2 (eta, phi) per texel), spectrum UAV (raw, 2 float2 per bin, pitch N + 1), output UAV
//        (texture: eta, eta_x, eta_z, phi), accumulation UAV (raw, int2 per texel)
//   P[1] dt (s, float), texel h (m), twiddle SRV (OceanFft.hlsli reads P[1].z), depth d (m; 0 = deep)
//   P[2] sources SRV (raw, RippleSource), source count, shift x, shift z (texels, int: the window moved by this)
//   P[3] g, sigma / rho (m^3/s^2), nu (m^2/s), sponge strength (1/s at the edge)

float rippleDt() { return asfloat(P[1].x); }
float rippleH() { return asfloat(P[1].y); }
float rippleDepth() { return asfloat(P[1].w); }
float2 rippleK(uint2 index)
{
    int2 m = int2(index);
    m = select(m >= int(RIPPLE_N / 2u), m - int(RIPPLE_N), m);
    return float2(m) * (2.0 * OCEAN_PI / (float(RIPPLE_N) * rippleH()));
}
bool rippleNyquist(uint2 index) { return index.x == RIPPLE_N / 2u || index.y == RIPPLE_N / 2u; }
// Sponge factor per frame at texel (x, z): 1 inside, exp(-s dt ramp^2) across the outer RIPPLE_SPONGE texels.
float rippleSponge(uint2 t)
{
    const float edge = float(min(min(t.x, RIPPLE_N - 1u - t.x), min(t.y, RIPPLE_N - 1u - t.y)));
    if (edge >= float(RIPPLE_SPONGE)) return 1.0;
    const float ramp = 1.0 - edge / float(RIPPLE_SPONGE);
    return exp(-asfloat(P[3].w) * rippleDt() * ramp * ramp);
}
struct RippleSource
{
    float2 position;  // window-relative texel coordinates of the centre (float)
    float radius;     // Gaussian sigma (m)
    float impulse;    // vertical impulse on the water (N s, positive = downward push)
};
#endif
