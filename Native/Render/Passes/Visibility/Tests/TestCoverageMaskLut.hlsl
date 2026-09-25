// unx-kernel: cs_6_6 main
// V test (coverage_mask_lut_matches_exact): random triangles against the unit pixel at (0, 0), their exact 32-subsample
// mask (coverageTriangleMask) and the LUT mask (coverageTriangleMaskLut) side by side.
//   P[0] LUT SRV (uint2), output UAV (raw, 12 B per case: exact, LUT, subsamples tested exactly), case count, seed
#include "Bindless.hlsli"
#include "Passes/Visibility/Coverage.hlsli"

uint hashCase(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float unitRandom(inout uint state)
{
    state = hashCase(state + 0x9E3779B9u);
    return (state >> 8) * (1.0 / 16777216.0);
}

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[0].z) return;
    uint state = hashCase(id * 3u + P[0].w);
    // Three size classes: vertices within 0.75 px of the pixel (slivers and small triangles cutting it), within 3 px,
    // and within 30 px (long edges crossing it).
    const uint sizeClass = id % 3;
    const float spread = sizeClass == 0 ? 0.75 : sizeClass == 1 ? 3.0 : 30.0;
    float2 v[3];
    for (uint k = 0; k < 3; ++k) v[k] = 0.5 + (float2(unitRandom(state), unitRandom(state)) * 2 - 1) * spread;
    StructuredBuffer<uint2> lut = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    // Subsamples the LUT path leaves to the exact test (its extra work), recomputed here the same way.
    const float orient = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    const float s = orient >= 0 ? 1.0 : -1.0;
    uint in0, am0, in1, am1, in2, am2;
    coverageEdgeLut(v[0], v[1], 0, s, lut, in0, am0);
    coverageEdgeLut(v[1], v[2], 0, s, lut, in1, am1);
    coverageEdgeLut(v[2], v[0], 0, s, lut, in2, am2);
    const uint tested = countbits((in0 | am0) & (in1 | am1) & (in2 | am2) & (am0 | am1 | am2));
    output.Store3(12 * id, uint3(coverageTriangleMask(v[0], v[1], v[2], 0), coverageTriangleMaskLut(v[0], v[1], v[2], 0, lut), tested));
}
