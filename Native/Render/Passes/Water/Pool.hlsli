// Closed basins W2 (bath, pool; FEATURES_GAME 1.10): the linear water surface of a rectangular basin Lx x Lz with
// reflecting walls, evolved every frame by the exact spectral rotation of Ripple.hlsli (gravity-capillary dispersion over
// the basin depth d, viscous decay exp(-2 nu k^2 t)).
// Walls: the basin's surface is sampled at the vertices x_i = i hx, z_j = j hz, i, j = 0..256 (hx = Lx / 256), including
// both walls. Its even mirror across both walls is a 512 x 512 periodic field whose Fourier modes are the basin's own
// modes cos(m pi x / Lx) cos(n pi z / Lz) (the discrete cosine transform of type I): each mode satisfies the no-flux wall
// condition d eta / dn = d phi / dn = 0 exactly, so a wave reflects off a wall with no loss and no phase error. The state
// is the modes' amplitudes (PoolEvolve): only sources are transformed forward (their frames), the output inverse; no
// float round trip feeds back into the state. The FFT of the mirrored field is OceanFft.hlsli's 512-point transform
// (the forward columns and inverse rows passes are Ripple's own kernels: same root constant slots for the spectrum and
// the twiddles).
// Volume: the basin's water volume is the trapezoid sum of eta over the samples (walls 1/2, corners 1/4) x hx hz; the
// mean (k = 0) is kept by the evolution and by the sources (PoolSplat).
// Damping of each mode (amplitude rate, exact linear theory): the bulk's 2 nu k^2 plus the laminar (Stokes) boundary
// layers on the floor and the four walls, and on the surface when it carries an inextensible film (bathers, soap):
// a solid surface dissipates (rho / 2) sqrt(nu w / 2) |U_t|^2 per unit area (U_t the inviscid tangential velocity
// amplitude there), the mode holds (rho / 2) integral |grad Phi|^2, so for Phi = cos(kx x) cos(kz z) cosh(k (y + d)):
//   delta = sqrt(nu w / 2) / (2 k) [ 2 k^2 / sinh(2 k d)                                     (floor)
//           + ax (kz^2 (d / s + 1 / 2k) + k^2 (1 / 2k - d / s))                          (walls x = 0, Lx)
//           + az (kx^2 (d / s + 1 / 2k) + k^2 (1 / 2k - d / s))                          (walls z = 0, Lz)
//           + film k^2 coth(k d) ],   s = sinh(2 k d), ax = (kx != 0 ? 4 : 2) / Lx, az = (kz != 0 ? 4 : 2) / Lz;
// deep water (d = 0): no floor, d / s = 0, coth = 1. It holds while the boundary layer sqrt(2 nu / w) is thin against
// the depth and the wavelength. A bath's sloshing mode (4 x 3 x 0.6 m) rings for 1 / delta ~ 16 min, bulk viscosity
// alone would leave it for days. The rotation (cos, sin) is normalised, so the per-mode gain is exactly the damping.
// Root constants shared by the pool kernels:
//   P[0] modes UAV (raw, float2 (H, Phi) per mode (m, n), 257 x 257), spectrum UAV (raw, Ripple layout: 2 float2 per
//        bin, pitch N + 1), output UAV (texture 257 x 257: eta, eta_x, eta_z, phi), accumulation UAV (raw, int2 per sample)
//   P[1] dt (s, float), previous-eta UAV (raw float per sample: the output before this evolution), twiddle SRV
//        (OceanFft.hlsli reads P[1].z), depth d (m; 0 = deep)
//   P[2] sources SRV (raw, PoolSourceGpu), source count, hx, hz (m)
//   P[3] g, sigma / rho (m^3/s^2), nu (m^2/s), the sources' mean-level term V_total / (Lx Lz) (m, float: PoolRows)
//   P[4] surface film (0: clean, 1: inextensible), Lx, Lz (m), flags (1: this evolution has an increment in the
//        spectrum; 2: it replaces the state - setState)
//   P[5] input field UAV (raw, float2 (eta, phi) per sample: setState's upload), mode table SRV (raw, float4 per mode:
//        w, K / w, w / K, delta in double rounded once: the GPU's tanh, sqrt and sinh would put their approximation
//        error into every mode's frequency)
#ifndef UNX_WATER_POOL_HLSLI
#define UNX_WATER_POOL_HLSLI
#define FFT_CHANNELS 2u
#include "OceanFft.hlsli"

#define POOL_N OCEAN_N           // the mirrored periodic domain
#define POOL_Q (OCEAN_N / 2u + 1u)  // samples per axis of the basin (walls included)
#define POOL_PITCH OCEAN_PITCH
#define POOL_FIXED 16777216.0    // source accumulation: 2^24 per unit (eta m, phi m^2/s), as Ripple's
#define POOL_RHO 1000.0
#define POOL_MAX_REACH 64

float poolDt() { return asfloat(P[1].x); }
float poolDepth() { return asfloat(P[1].w); }
float2 poolH() { return asfloat(P[2].zw); }
// Mirrored index (0..511) -> basin sample (0..256).
uint poolFold(uint i) { return i <= POOL_N / 2u ? i : POOL_N - i; }
// sin and cos of x to float rounding (the rotation angle w dt of every mode, every frame): the hardware pair carries an
// absolute error near 5e-7 that repeats every frame (a coherent phase drift, measured 1.5e-4 rad after 300 frames).
// Cody-Waite reduction by pi / 2 (three-part constant) and Taylor polynomials on [-pi/4, pi/4] (first omitted terms
// r^11 / 11!, r^10 / 10! below 2e-9).
void poolSinCos(float x, out float s, out float c)
{
    const float k = round(x * 0.63661977236758134);
    const float r = ((x - k * 1.5703125) - k * 4.8375129699707031e-4) - k * 7.5497899548918822e-8;
    const float r2 = r * r;
    const float sr = r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880))));
    const float cr = 1 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320))));
    const int q = int(k) & 3;
    s = q == 0 ? sr : (q == 1 ? cr : (q == 2 ? -sr : -cr));
    c = q == 0 ? cr : (q == 1 ? -sr : (q == 2 ? -cr : sr));
}
struct PoolSourceGpu  // 32 B
{
    float2 position;  // basin sample coordinates of the centre (0..256 per axis inside the basin)
    float radius;     // Gaussian sigma (m)
    float impulse;    // vertical impulse on the water (N s, positive = downward push)
    float volume;     // displaced volume (m^3, positive = water pushed out of the footprint)
    float3 reserved;
};
#endif
