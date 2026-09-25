// unx-kernel: cs_6_6 main
// unx-variants: PASS=0,1,2,3,4,5
// Exact multiple-scattering source table J_ms (AtmosphereCommon.hlsli, S_STATUS_KO.md 8) by iterating scattering orders
// (Bruneton 2008 structure) on the table's own 4D grid (nu, mu_s, mu, r). Per unit solar illuminance; L_n is the
// radiance of light scattered n times (the ground's reflections counted with the order of the light they reflect),
// J_n the source of order n per unit sigma_s (sigma-weighted Rayleigh / Mie phase average):
//   PASS 0  L_1 = single scattering along the ray + the ground's reflection of the direct sun.
//   PASS 3  E_n(mu_s) = irradiance of L_n on the ground (entries [0, W)), summed into [W, 2W).
//   PASS 5  For every (r, mu_s) slice, L_{n-1} over the sphere of directions (it depends on the direction only through
//           mu and nu: symmetric about the sun's vertical plane) projected onto real spherical harmonics with cosine
//           azimuth terms, l <= L (multiscatter_sh_order): an elevation x azimuth grid split at the horizon with
//           square-root spacing towards it (the bright band of long grazing rays: the limb seen from altitude, the haze
//           near the ground; the table's own mu rows), azimuths over [0, pi] doubled (multiscatter_sh_grid).
//   PASS 1  J_n(v) = (sigma_R S_R + sigma_M S_M) / (sigma_R + sigma_M), S_i = sphere integral of p_i(w.v) L_{n-1}(w),
//           by Funk-Hecke: S_i = sum_lm lambda_l L_lm Y_lm(v) with lambda_l = g^l (Henyey-Greenstein, exact up to the
//           truncation g^(L+1): 6e-4 at L = 32, g = 0.8) and lambda_0 = 1, lambda_2 = 1/10 (Rayleigh: exact).
//           Accumulated into J_acc. Tables are log-domain (AtmosphereCommon.hlsli); J_acc and E are linear sums.
//   PASS 2  L_n = integral along the ray of T (sigma_R + sigma_M) J_n + T times the ground's reflection of E_{n-1}.
//   PASS 4  final table = ln(J_acc + geometric tail J_N q / (1 - q), q = min(0.9, J_N / J_{N-1})), and
//           the ground's indirect irradiance (sum of E_n) into the transmittance LUT's row size.y.
// Sequence (AtmosphereSystem.cpp): 0, 3, then for n = 2..N: 5, 1, 2, 3; then 4. Rays march with quadratic spacing and
// exact exponentials per step (midpoint source), like the reference (AtmosphereReference.cpp). Deterministic: fixed
// quadratures and summation orders. The accumulators (J_acc, E) are structured buffers (read-modify-write without
// typed UAV loads).
// P[0].x params (raw), P[0].y transmittance LUT (SRV; PASS 4: UAV), P[1].w the r slice of the dispatch (PASS 0, 1, 2,
// 4, 5), other slots per pass (see each block).
// Dispatch, one per r slice (each dispatch stays short whatever the counts: no device timeout): PASS 0, 2, 4
// ceil(N_nu N_mus / 64) x N_mu x 1 groups; PASS 1 and 5 N_mus groups (one per (r, mu_s) slice); PASS 3
// ceil(transmittanceSize.x / 64) groups.
#include "Bindless.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

#define MS_SH_MAX 48u  // largest multiscatter_sh_order the kernels hold (registers of PASS 5)
groupshared float3 gs_coefficients[(MS_SH_MAX + 1) * (MS_SH_MAX + 2) / 2];
groupshared float4 gs_node[64];
groupshared float2 gs_nodeX[64];

struct MsTexel
{
    float altitude, mu, mus, nu;
    float3 origin, v, sun;
};

// The texel's geometry in a local frame (y up at the point, v in the xy plane). nu is clamped to the range the other
// two cosines allow.
MsTexel msTexel(AtmosphereParams a, uint3 id)
{
    const uint4 n = a.multiScatterSize;
    MsTexel t;
    t.altitude = airMsAltitude(a, float(id.z) / (n.w - 1));
    t.mu = airMsViewCosine(a, t.altitude, id.y);
    t.mus = airMsSunCosine(float(id.x % n.y) / (n.y - 1));
    const float sm = sqrt(saturate(1 - t.mu * t.mu)), ss = sqrt(saturate(1 - t.mus * t.mus));
    t.nu = clamp(airMsNu(float(id.x / n.y) / (n.x - 1)), t.mu * t.mus - sm * ss, t.mu * t.mus + sm * ss);
    t.origin = float3(0, t.altitude, 0);
    t.v = float3(sm, t.mu, 0);
    const float sx = sm > 1e-6 ? clamp((t.nu - t.mu * t.mus) / sm, -ss, ss) : ss;
    t.sun = float3(sx, t.mus, sqrt(saturate(1 - t.mus * t.mus - sx * sx)));
    return t;
}

bool msInside(AtmosphereParams a, uint3 id) { return id.x < a.multiScatterSize.x * a.multiScatterSize.y; }
// Texture coordinate of a texel id (x = i_nu N_mus + i_mus, y = i_mu, z = i_r): (i_mus, i_mu, i_nu N_r + i_r).
uint3 msTexelCoord(AtmosphereParams a, uint3 id)
{
    const uint4 n = a.multiScatterSize;
    return uint3(id.x % n.y, id.y, (id.x / n.y) * n.w + id.z);
}
uint msIndex(AtmosphereParams a, uint3 id) { return (id.z * a.multiScatterSize.z + id.y) * (a.multiScatterSize.x * a.multiScatterSize.y) + id.x; }

// Ground irradiance of the previous order from E (entries [0, transmittanceSize.x): E_{n-1}; the next as many: the sum),
// at the ground point's sun cosine.
float3 msGroundIrradiance(AtmosphereParams a, StructuredBuffer<float4> e, float mus)
{
    const float q = airMsSunCoord(clamp(mus, -1.0, 1.0)) * (a.transmittanceSize.x - 1);
    const uint i0 = min((uint)q, a.transmittanceSize.x - 2);
    return lerp(e[i0].rgb, e[i0 + 1].rgb, q - i0);
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint3 id = uint3(thread.xy, thread.z + P[1].w);
    const AtmosphereParams a = airLoadParams(P[0].x);
    const uint tlut = P[0].y;
#if PASS == 0
    // P[0].z L_1 UAV (RWTexture3D<float4>, RGBA32F)
    if (!msInside(a, id)) return;
    const MsTexel t = msTexel(a, id);
    const float2 span = airInterval(a, t.origin, t.v, 3.402823466e38);
    const float extent = max(0.0, span.y - span.x);
    const float pr = airRayleighPhase(t.nu), pm = airMiePhase(t.nu, a.mieG);
    const uint steps = a.multiScatterSteps;
    float3 L = 0, T = 1;
    [loop] for (uint j = 0; j < steps; ++j)
    {
        const float u0 = float(j) / steps, u1 = float(j + 1) / steps;
        const float t0 = extent * u0 * u0, step = extent * (u1 * u1 - u0 * u0);
        const float3 p = t.origin + t.v * (span.x + t0 + 0.5 * step);
        const AirCoefficients c = airCoefficients(a, airAltitude(a, p));
        L += T * airIntegral(c.extinction, step) * (c.rayleigh * pr + c.mie * pm) * airSunTransmittance(a, tlut, p, t.sun);
        T *= exp(-c.extinction * step);
    }
    if (airHitsGround(a, t.origin, t.v))
    {
        const float3 g = t.origin + t.v * span.y, up = airUp(a, g);
        L += T * a.groundAlbedo * (saturate(dot(up, t.sun)) / ATMO_PI) * airSunTransmittance(a, tlut, g + up * 0.01, t.sun);
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].z];
    output[msTexelCoord(a, id)] = float4(airMsEncode(L), 0);
#elif PASS == 1
    // P[0].z coefficients SRV (StructuredBuffer<float4>, PASS 5), P[0].w J_n UAV, P[1].x J_acc UAV (RWStructuredBuffer<float4>),
    // P[1].y SH order L, P[1].z 1: first order (J_acc = J_n). Group = the slice (mu_s = group x, r = P[1].w).
    const uint4 n = a.multiScatterSize;
    const uint L = min(P[1].y, MS_SH_MAX), count = (L + 1) * (L + 2) / 2;
    const uint iMus = thread.x / 64, lane = thread.x % 64, r = P[1].w;
    StructuredBuffer<float4> coefficients = ResourceDescriptorHeap[P[0].z];
    // The slice's coefficients weighted by the phase eigenvalues and the altitude's sigma_R : sigma_M (per channel).
    {
        const float h = min(airMsAltitude(a, float(r) / (n.w - 1)), (a.topRadius - a.bottomRadius) * 0.999999);
        const AirCoefficients c = airCoefficients(a, h);
        const float3 sigma = c.rayleigh + c.mie;
        const float3 wR = select(sigma > 0, c.rayleigh / max(sigma, 1e-30), 1.0), wM = select(sigma > 0, c.mie / max(sigma, 1e-30), 0.0);
        const uint base = (r * n.y + iMus) * count;
        for (uint i = lane; i < count; i += 64)
        {
            uint l = 0;
            while ((l + 1) * (l + 2) / 2 <= i) ++l;
            const float rayleigh = l == 0 ? 1.0 : (l == 2 ? 0.1 : 0.0);
            float gl = 1;
            for (uint e = 0; e < l; ++e) gl *= a.mieG;
            gs_coefficients[i] = coefficients[base + i].rgb * (wR * rayleigh + wM * gl);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].w];
    RWStructuredBuffer<float4> acc = ResourceDescriptorHeap[P[1].x];
    for (uint k = lane; k < n.x * n.z; k += 64)
    {
        const uint3 texel = uint3((k % n.x) * n.y + iMus, k / n.x, r);
        const MsTexel t = msTexel(a, texel);
        const float sm = sqrt(saturate(1 - t.mu * t.mu)), ss = sqrt(saturate(1 - t.mus * t.mus));
        const float cphi = sm * ss > 1e-6 ? clamp((t.nu - t.mu * t.mus) / (sm * ss), -1.0, 1.0) : 1.0;
        float3 J = 0;
        // Y_lm(v) = P~_l^m(mu) (m > 0: sqrt 2 cos m phi); cos m phi by the Chebyshev recurrence.
        float cm = 1, cmPrev = cphi;  // cos(m phi), cos((m - 1) phi)
        float pmm = 0.28209479177387814;  // P~_0^0 = 1 / sqrt(4 pi)
        for (uint m = 0; m <= L; ++m)
        {
            if (m > 0)
            {
                pmm *= -sqrt((2.0 * m + 1) / (2.0 * m)) * sm;
                const float next = 2 * cphi * cm - cmPrev;
                cmPrev = cm;
                cm = next;
            }
            const float azimuth = m == 0 ? 1.0 : 1.4142135623730951 * cm;
            float p2 = 0, p1 = pmm;
            J += gs_coefficients[m * (m + 1) / 2 + m] * (p1 * azimuth);
            for (uint l = m + 1; l <= L; ++l)
            {
                const float p = l == m + 1 ? sqrt(2.0 * m + 3) * t.mu * pmm
                                           : sqrt((4.0 * l * l - 1) / (float(l * l) - float(m * m))) *
                                                 (t.mu * p1 - sqrt((float((l - 1) * (l - 1)) - float(m * m)) / (4.0 * (l - 1) * (l - 1) - 1)) * p2);
                p2 = p1;
                p1 = p;
                J += gs_coefficients[l * (l + 1) / 2 + m] * (p * azimuth);
            }
        }
        J = max(J, 0.0);  // the truncated series of a non-negative convolution (ringing below 6e-4 of the lobe)
        output[msTexelCoord(a, texel)] = float4(airMsEncode(J), 0);
        const uint i = msIndex(a, texel);
        acc[i] = float4(P[1].z != 0 ? J : acc[i].rgb + J, 0);
    }
#elif PASS == 2
    // P[0].z J_n SRV (Texture3D<float4>), P[0].w L_n UAV, P[1].x E SRV (StructuredBuffer<float4>)
    if (!msInside(a, id)) return;
    const MsTexel t = msTexel(a, id);
    Texture3D<float4> source = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<float4> irradiance = ResourceDescriptorHeap[P[1].x];
    const float2 span = airInterval(a, t.origin, t.v, 3.402823466e38);
    const float extent = max(0.0, span.y - span.x);
    const uint steps = a.multiScatterSteps;
    const float top = a.topRadius - a.bottomRadius;
    float3 L = 0, T = 1;
    [loop] for (uint j = 0; j < steps; ++j)
    {
        const float u0 = float(j) / steps, u1 = float(j + 1) / steps;
        const float t0 = extent * u0 * u0, step = extent * (u1 * u1 - u0 * u0);
        const float3 p = t.origin + t.v * (span.x + t0 + 0.5 * step);
        const float hp = airAltitude(a, p);
        const AirCoefficients c = airCoefficients(a, hp);
        const float3 up = airUp(a, p);
        const float3 J = airMsSample(a, source, clamp(hp, 0.0, top), dot(up, t.v), dot(up, t.sun), t.nu);
        L += T * airIntegral(c.extinction, step) * (c.rayleigh + c.mie) * J;
        T *= exp(-c.extinction * step);
    }
    if (airHitsGround(a, t.origin, t.v))
    {
        const float3 g = t.origin + t.v * span.y, up = airUp(a, g);
        L += T * a.groundAlbedo / ATMO_PI * msGroundIrradiance(a, irradiance, dot(up, t.sun));
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].w];
    output[msTexelCoord(a, id)] = float4(airMsEncode(L), 0);
#elif PASS == 3
    // P[0].z L_n SRV (Texture3D<float4>), P[0].w E UAV (RWStructuredBuffer<float4>, 2 transmittanceSize.x), P[1].x 1:
    // first order (sum = E_n), P[1].y hemisphere directions
    if (id.x >= a.transmittanceSize.x) return;
    Texture3D<float4> radiance = ResourceDescriptorHeap[P[0].z];
    const float mus = airMsSunCosine(float(id.x) / (a.transmittanceSize.x - 1));
    const float3 sun = float3(sqrt(saturate(1 - mus * mus)), mus, 0);
    const uint directions = max(1u, P[1].y);
    float3 E = 0;
    [loop] for (uint i = 0; i < directions; ++i)
    {
        // Uniform in solid angle over the upper hemisphere (z uniform in (0, 1)), cosine-weighted sum.
        const float z = 1 - (i + 0.5) / directions, phi = i * 2.399963229728653, s = sqrt(saturate(1 - z * z));
        const float3 w = float3(s * cos(phi), z, s * sin(phi));
        E += z * airMsSample(a, radiance, 0.0, z, mus, dot(w, sun));
    }
    E *= 6.283185307179586 / directions;
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
    const uint sum = a.transmittanceSize.x + id.x;
    output[id.x] = float4(E, 0);
    output[sum] = float4(P[1].x != 0 ? E : output[sum].rgb + E, 0);
#elif PASS == 5
    // P[0].z L_{n-1} SRV (Texture3D<float4>), P[0].w coefficients UAV (RWStructuredBuffer<float4>, per slice (L + 1)(L + 2) / 2),
    // P[1].x SH order L, P[1].y elevation nodes per half K, P[1].z azimuth nodes over [0, pi] A. Group = the slice;
    // thread m accumulates the coefficients (l, m), l = m..L, in registers over the grid, 64 nodes at a time.
    const uint4 n = a.multiScatterSize;
    const uint L = min(P[1].x, MS_SH_MAX), K = P[1].y, A = P[1].z;
    const uint iMus = thread.x / 64, lane = thread.x % 64, r = P[1].w;
    Texture3D<float4> radiance = ResourceDescriptorHeap[P[0].z];
    const float altitude = airMsAltitude(a, float(r) / (n.w - 1));
    const float mus = airMsSunCosine(float(iMus) / (n.y - 1)), ss = sqrt(saturate(1 - mus * mus));
    const float hz = airHorizonElevation(a, altitude);
    const uint m = lane;
    float kmm = 0.28209479177387814;  // |P~_m^m| / sin^m: sqrt((2m + 1) / 4 pi x prod (2k - 1) / 2k), sign (-1)^m
    for (uint k2 = 1; k2 <= m; ++k2) kmm *= sqrt((2.0 * k2 + 1) / (2.0 * k2));
    if (m & 1) kmm = -kmm;
    float3 acc[MS_SH_MAX + 1];
    [unroll] for (uint j0 = 0; j0 <= MS_SH_MAX; ++j0) acc[j0] = 0;
    const uint nodes = 2 * K * A;
    for (uint first = 0; first < nodes; first += 64)
    {
        const uint j = first + lane;
        float4 node = 0;  // (L rgb x weight, and x = sin el in gs_node_x, cos phi in gs_node_c)
        float x = 0, cphi = 1;
        if (j < nodes)
        {
            const uint above = j / (K * A), k = (j / A) % K;
            const float u = (k + 0.5) / K, range = above ? 1.5707963267948966 - hz : hz + 1.5707963267948966;
            const float el = above ? hz + u * u * range : hz - u * u * range;
            const float phi = ATMO_PI * ((j % A) + 0.5) / A;
            x = sin(el);
            cphi = cos(phi);
            const float weight = cos(el) * 2 * u * range / K * (6.283185307179586 / A);
            node = float4(airMsSample(a, radiance, altitude, x, mus, cos(el) * cphi * ss + x * mus) * weight, 0);
        }
        gs_node[lane] = node;
        gs_nodeX[lane] = float2(x, cphi);
        GroupMemoryBarrierWithGroupSync();
        if (m <= L)
            for (uint q = 0; q < 64 && first + q < nodes; ++q)
            {
                const float3 value = gs_node[q].rgb;
                const float xq = gs_nodeX[q].x, cq = gs_nodeX[q].y;
                const float sq = sqrt(saturate(1 - xq * xq));
                // cos(m phi) by the Chebyshev recurrence (m <= L <= 64).
                float cm = 1, cprev = cq;
                for (uint t2 = 0; t2 < m; ++t2)
                {
                    const float next = 2 * cq * cm - cprev;
                    cprev = cm;
                    cm = next;
                }
                const float azimuth = m == 0 ? 1.0 : 1.4142135623730951 * cm;
                const float pmm = kmm * pow(sq, float(m));
                float p2 = 0, p1 = pmm;
                [unroll] for (uint i2 = 0; i2 <= MS_SH_MAX; ++i2)
                {
                    const uint l = m + i2;
                    if (l > L) break;
                    float p = pmm;
                    if (i2 == 1) p = sqrt(2.0 * m + 3) * xq * pmm;
                    else if (i2 > 1)
                        p = sqrt((4.0 * l * l - 1) / (float(l * l) - float(m * m))) *
                            (xq * p1 - sqrt((float((l - 1) * (l - 1)) - float(m * m)) / (4.0 * (l - 1) * (l - 1) - 1)) * p2);
                    if (i2 > 0)
                    {
                        p2 = p1;
                        p1 = p;
                    }
                    acc[i2] += value * (p * azimuth);
                }
            }
        GroupMemoryBarrierWithGroupSync();
    }
    if (m <= L)
    {
        RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
        const uint base = (r * n.y + iMus) * ((L + 1) * (L + 2) / 2);
        [unroll] for (uint i3 = 0; i3 <= MS_SH_MAX; ++i3)
        {
            const uint l = m + i3;
            if (l > L) break;
            output[base + l * (l + 1) / 2 + m] = float4(acc[i3], 0);
        }
    }
#else
    // P[0].y transmittance LUT UAV, P[0].z J_acc SRV (StructuredBuffer<float4>), P[0].w J_N SRV, P[1].x J_{N-1} SRV
    // (0xFFFFFFFF: N = 2, no tail), P[1].y final table UAV (RWTexture3D<float4>, R16G16B16A16_UNORM), P[1].z E SRV (the sum)
    if (all(id.yz == 0) && id.x < a.transmittanceSize.x)
    {
        StructuredBuffer<float4> irradiance = ResourceDescriptorHeap[P[1].z];
        RWTexture2D<float4> lut = ResourceDescriptorHeap[P[0].y];
        lut[uint2(id.x, a.transmittanceSize.y)] = float4(irradiance[a.transmittanceSize.x + id.x].rgb, 0);
    }
    if (!msInside(a, id)) return;
    StructuredBuffer<float4> acc = ResourceDescriptorHeap[P[0].z];
    Texture3D<float4> last = ResourceDescriptorHeap[P[0].w];
    float3 J = acc[msIndex(a, id)].rgb;
    if (P[1].x != 0xFFFFFFFFu)
    {
        Texture3D<float4> before = ResourceDescriptorHeap[P[1].x];
        const int4 tc = int4(msTexelCoord(a, id), 0);
        const float3 jn = airMsDecode(last.Load(tc).rgb), jp = airMsDecode(before.Load(tc).rgb);
        const float3 q = min(select(jp > 0, jn / max(jp, 1e-30), 0.0), 0.9);
        J += jn * q / (1 - q);
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[1].y];
    output[msTexelCoord(a, id)] = float4(airMsEncode(J), 0);
#endif
}
