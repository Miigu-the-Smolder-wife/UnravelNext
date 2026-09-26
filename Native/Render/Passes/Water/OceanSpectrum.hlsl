// unx-kernel: cs_6_6 main
// Ocean initial spectrum (Ocean.hlsli), once per sea state: per cascade and bin, h0(k) and conj(h0(-k)) (the pair that
// h(k, t) needs), with the cascade's band applied (cascade c holds k_c <= |k| < k_{c+1}, k_0 = 0, k_3 = infinity).
// Root constants: P[0] h0 UAV (raw, float4 per bin), 0, seed, cascade count; P[1] wind speed U (m/s), fetch F (m),
// wind unit vector (x, z); P[2] cascade lengths (m) 0..2; P[3] band boundaries k1, k2 (1/m), the spreading
// normalisation Gamma(s + 1) / (2 sqrt(pi) Gamma(s + 1/2)), spread s.
#include "Ocean.hlsli"

uint oceanHash(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float oceanUniform(uint h) { return (float(h >> 9) + 0.5) / 8388608.0; }  // (0, 1), exact in float (24 significant bits)
// -log(u) to a few ulps relative on (0, 1): the hardware log's absolute error would dominate the small radii of
// Box-Muller as u -> 1, so there log(u) = 2 atanh(t), t = (u - 1) / (u + 1) (u - 1 exact), by its odd series
// (|t| <= 1/3: 10 terms reach 2^-32).
float oceanNegLog(float u)
{
    if (u < 0.5) return -log(u);
    float t = (u - 1.0) / (u + 1.0), t2 = t * t, s = 1.0 / 19.0;
    [unroll] for (int n = 8; n >= 0; --n) s = 1.0 / float(2 * n + 1) + t2 * s;
    return -2.0 * t * s;
}
// Standard complex normal (Box-Muller) for (seed, cascade, bin): deterministic, independent of dispatch order.
float2 oceanGaussian(uint cascade, uint bin)
{
    uint a = oceanHash(P[0].z ^ oceanHash(cascade * 0x9E3779B9u ^ oceanHash(bin))), b = oceanHash(a ^ 0x68E31DA4u);
    float r = sqrt(2.0 * oceanNegLog(oceanUniform(a))), phi = 2.0 * OCEAN_PI * oceanUniform(b);
    return r * float2(cos(phi), sin(phi));
}
[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    uint cascades = P[0].w;
    if (i >= OCEAN_N * OCEAN_N * cascades) return;
    uint cascade = i / (OCEAN_N * OCEAN_N), bin = i % (OCEAN_N * OCEAN_N);
    uint2 index = uint2(bin % OCEAN_N, bin / OCEAN_N), mirror = uint2((OCEAN_N - index.x) % OCEAN_N, (OCEAN_N - index.y) % OCEAN_N);
    float L = oceanLength(cascade);
    RWByteAddressBuffer h0 = ResourceDescriptorHeap[P[0].x];
    if (index.x == 0 || index.y == 0) { h0.Store4(16 * i, 0); return; }  // Nyquist (Ocean.hlsli)
    float2 h = oceanGaussian(cascade, bin) * sqrt(0.25 * oceanVariance(oceanK(index, L), L, cascade));
    float2 hm = oceanGaussian(cascade, mirror.y * OCEAN_N + mirror.x) * sqrt(0.25 * oceanVariance(oceanK(mirror, L), L, cascade));
    h0.Store4(16 * i, asuint(float4(h, hm.x, -hm.y)));
}
