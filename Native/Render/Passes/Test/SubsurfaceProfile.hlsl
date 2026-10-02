// unx-kernel: cs_6_6 main
// Core test kernel (Subsurface class, stage B): Passes/Common/SubsurfaceProfile.hlsli at deterministic points - albedo,
// mean free path, distances and sample parameters from the thread index. Per point 24 floats into a raw UAV: albedo (3),
// mean free path (3), r, h, xi, u, k, pairs, d (3), p(d.x, r), P(d.x, r), P^-1 of the widest channel at xi, the sample
// radius and angle of pair k, the sample weight (3) at (r, h), the centre's P used for the sample radius.
//   P[0].x output UAV (raw), P[0].y point count
#include "Bindless.hlsli"
#include "Passes/Common/SubsurfaceProfile.hlsli"

float hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const float3 albedo = float3(hash01(i * 7 + 1), hash01(i * 7 + 2), hash01(i * 7 + 3));
    // mean free paths from 0.1 mm to 10 cm (log-uniform); every eighth point without one in the blue channel
    float3 meanFreePath = 1e-4 * pow(1000.0, float3(hash01(i * 7 + 4), hash01(i * 7 + 5), hash01(i * 7 + 6)));
    if ((i % 8) == 7) meanFreePath.b = 0;
    const float3 d = sssDistance(meanFreePath, albedo);
    const float dS = max(d.r, max(d.g, d.b));
    const float r = dS * 12.0 * hash01(i * 11 + 5) * hash01(i * 11 + 6);  // up to 12 d, denser near 0
    const float h = (i % 3) == 0 ? 0.0 : dS * 3.0 * hash01(i * 13 + 7);
    const float xi = hash01(i * 17 + 9), u = hash01(i * 19 + 11), centre = 0.9 * hash01(i * 23 + 13);
    const uint pairs = 1 + i % 32, k = (i / 32) % pairs;
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint b = 96 * i;
    o.Store3(b, asuint(albedo));
    o.Store3(b + 12, asuint(meanFreePath));
    o.Store4(b + 24, uint4(asuint(r), asuint(h), asuint(xi), asuint(u)));
    o.Store2(b + 40, uint2(asuint(float(k)), asuint(float(pairs))));
    o.Store3(b + 48, asuint(d));
    o.Store4(b + 60, uint4(asuint(sssRadialPdf(d.x, r)), asuint(sssRadialCdf(d.x, r)), asuint(sssRadius(dS, xi)), asuint(sssSampleRadius(dS, centre, k, pairs, u))));
    o.Store(b + 76, asuint(sssSampleAngle(k, u)));
    o.Store3(b + 80, asuint(sssSampleWeight(d, r, h, sssRadialPdf(dS, r))));
    o.Store(b + 92, asuint(centre));
}
