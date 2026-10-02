// unx-kernel: cs_6_6 main
// Core test kernel (Hair class): Passes/Hair/HairScattering.hlsli at deterministic points - the outgoing and incoming
// directions, the fibre counts and the spread from the thread index, the fibre from the scene's material records
// P[0].z .. P[0].z + P[0].w - 1 (Hair class: eta = ior, beta_M = roughness, the record's sigma_a, beta_N and tilt).
// Per point 48 floats into a raw UAV: wo, wi, count in front, count behind, spread, 0; the width-averaged kernel, plain
// and spread over the forward half circle; the averages (a_f, a_b, the two variances, the two shifts) at the incoming
// inclination; what the count in front lets through (direct, scattered, spread); the backward lobe (A_b, shift,
// variance); hairStrandLight; the moments (albedo, along, across) with 1 - exp(-count behind).
//   P[0].x output UAV (raw), P[0].y point count, P[0].z first material, P[0].w material count
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Hair/HairScattering.hlsli"

float hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

float3 direction(uint seed)
{
    const float z = 0.98 * (2 * hash01(seed) - 1), phi = 6.2831853 * hash01(seed + 1), r = sqrt(1 - z * z);
    return float3(z, r * cos(phi), r * sin(phi));
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const float3 wo = direction(8 * i + 1), wi = direction(8 * i + 3);
    const float front = (i % 4) == 0 ? 0.0 : 4 * hash01(8 * i + 5), behind = (i % 5) == 0 ? 0.0 : 3 * hash01(8 * i + 6), spread = 0.3 * hash01(8 * i + 7);
    const GpuMaterial m = loadMaterial(P[0].z + i % P[0].w);
    const HairStrand s = hairStrand(wo, m.ior, m.hairAbsorption, m.roughness, m.hairBetaN, m.hairTilt);
    const HairAverage a = hairAverage(wi.x, m.ior, m.hairAbsorption, m.roughness, m.hairBetaN, m.hairTilt);
    const HairThrough t = hairThrough(a, front);
    const HairBack b = hairBackscatter(a);
    const HairMoments mo = hairStrandMoments(s, a, 1 - exp(-behind));
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint at = 192 * i;
    o.Store3(at, asuint(wo));
    o.Store3(at + 12, asuint(wi));
    o.Store4(at + 24, uint4(asuint(front), asuint(behind), asuint(spread), 0));
    o.Store3(at + 40, asuint(hairStrandKernel(s, wi, 0, false)));
    o.Store3(at + 52, asuint(hairStrandKernel(s, wi, spread, true)));
    o.Store3(at + 64, asuint(a.forward));
    o.Store3(at + 76, asuint(a.backward));
    o.Store4(at + 88, uint4(asuint(a.varianceForward), asuint(a.varianceBackward), asuint(a.shiftForward), asuint(a.shiftBackward)));
    o.Store(at + 104, asuint(t.direct));
    o.Store3(at + 108, asuint(t.scattered));
    o.Store(at + 120, asuint(t.spread));
    o.Store3(at + 124, asuint(b.albedo));
    o.Store2(at + 136, uint2(asuint(b.shift), asuint(b.variance)));
    o.Store3(at + 144, asuint(hairStrandLight(s, a, wi, front, behind)));
    o.Store3(at + 156, asuint(mo.albedo));
    o.Store3(at + 168, asuint(mo.along));
    o.Store3(at + 180, asuint(mo.across));
}
