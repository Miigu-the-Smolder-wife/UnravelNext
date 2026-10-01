// Round basins W2-R (defect queue 13 (74); include/unx/water/RoundPool.h): the kernels' shared layout.
//   grid:      N_theta = 512 angles theta_i = 2 pi i / 512, N_r = 128 rings r_j = (j + 1) R / 128 (the last on the wall)
//              and the centre; sample index i + j * 512
//   modes:     orders m = 0 .. 255, per order 'count' radial modes (host RoundTables: J_m'(k R) = 0, k R <= pi N_r), mode
//              index = orders[m].start + n; per mode float4 (H re, H im, Phi re, Phi im) (negative orders by conjugation)
//   spectrum:  per ring j and order m: float4 x 3 at (j * 256 + m): (H, Phi), (slope_r, 0), (unused) - the angular FFT's
//              bins (forward) or the synthesis' values (inverse)
//   tables:    analysis[orders[m].analysis + n * 128 + j] = F_m[n][j]; synthesis[orders[m].synthesis + j * count + n] =
//              B_m[j][n] = J_m(k_mn r_j); slope: the same layout with k J_m'(k r_j)
//   orders:    per order m: uint4 (start, count, analysis offset, synthesis offset)
//   output:    RGBA32F texture 512 x 128: (eta, eta_r, eta_theta / r, phi); centre buffer: float4 (eta, phi, previous eta, 0)
// Root constants (every round kernel):
//   P[0] modes UAV (raw), increments UAV (raw), spectrum UAV (raw), accumulation UAV (raw, int2 per sample, 2^24 fixed)
//   P[1] dt (float), previous-eta UAV (raw), twiddles SRV (OceanFft.hlsli reads P[1].z), orders SRV (raw)
//   P[2] sources SRV (raw, RoundSourceGpu), source count, R (m), depth d (m; 0 deep)
//   P[3] mode table SRV (raw float4: w, K / w, w / K, delta), analysis SRV (raw), synthesis SRV (raw), slope SRV (raw)
//   P[4] output UAV (texture), centre UAV (raw), flags (1: an increment this evolution), mean-level term (float, m)
#ifndef UNX_WATER_ROUND_POOL_HLSLI
#define UNX_WATER_ROUND_POOL_HLSLI
#define FFT_CHANNELS 4u
#include "OceanFft.hlsli"

#define ROUND_RHO 1000.0
#define ROUND_FIXED 16777216.0   // source accumulation: 2^24 per unit (eta m, phi m^2/s), as Pool.hlsli
// sin and cos of x to float rounding (Pool.hlsli poolSinCos, repeated here: Pool.hlsli fixes FFT_CHANNELS at 2): Cody-Waite
// reduction by pi / 2 and Taylor polynomials on [-pi/4, pi/4].
void roundSinCos(float x, out float s, out float c)
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

#define ROUND_THETA 512u
#define ROUND_RINGS 128u
#define ROUND_ORDERS 256u
#define ROUND_SAMPLES (ROUND_THETA * ROUND_RINGS)
#define ROUND_MAX_REACH 64

float roundRadius() { return asfloat(P[2].z); }
float roundRingSpacing() { return roundRadius() / float(ROUND_RINGS); }
float roundRingRadius(uint j) { return (float(j) + 1.0) * roundRingSpacing(); }
uint4 roundOrder(uint m) { ByteAddressBuffer o = ResourceDescriptorHeap[P[1].w]; return o.Load4(16 * m); }
struct RoundSourceGpu  // 32 B
{
    float2 position;  // basin-local metres (x, z) of the centre, from the basin's centre
    float radius;     // Gaussian sigma (m)
    float impulse;    // N s (+ down)
    float volume;     // m^3 (+ pushed out)
    float3 reserved;
};
#endif
