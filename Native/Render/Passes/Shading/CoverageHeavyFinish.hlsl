// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Coverage composite, stage F3 (CoverageShade.hlsli): one thread per heavy pixel after the rounds: the band A surface
// takes what the fragments left (covBandA), and the sum is tone mapped once into the colour target. A pixel the rounds
// left unfinished (more than COV_ROUNDS x COV_ROUND fragments needed: a defect) sets COV_M_ERROR_ROUNDS.
// P[0] = { state UAV (raw), heavy records (raw), heavy capacity, 0 }, P[1] = { particle layer, particle edge blocks (UNX_NONE:
// none), 0, colour UAV }, P[2] = { band A radiance, resolved or UNX_NONE, edge tile mask or UNX_NONE, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer heavy = ResourceDescriptorHeap[P[0].y];
    const uint h = (gid.x + gid.y * 65535) * 64 + gi;
    if (h >= min(state.Load(4 * COVS_HEAVY), P[0].z)) return;
    const uint base = 4 * h * COVH_WORDS;
    const uint pixelWord = heavy.Load(base);
    const uint2 pixel = uint2(pixelWord & 0xFFFFu, pixelWord >> 16);
    const float used = asfloat(heavy.Load(base + 4 * COVH_USED));
    if (heavy.Load(base + 4 * COVH_DONE) == 0) state.InterlockedOr(4 * COVS_ERRORS, COV_M_ERROR_ROUNDS);
    const float3 sum = asfloat(heavy.Load3(base + 4 * COVH_SUM)) + max(1 - used, 0.0) * covBandA(pixel);
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].w];
    color[pixel] = shEncodeExposed(shParticles(sum, pixel, P[1].x, P[1].y));  // P[1].xy particle layer
}
