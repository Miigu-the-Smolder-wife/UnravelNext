// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Coverage composite, compact form (shading.coverage_compact; CoverageShade.hlsli), stage G: one group of 64 threads per
// listed tile, one pixel per lane. A light pixel sums its entries' weight x radiance (CoverageShadeList's two parts) in
// the walk's order, the band A surface takes what the fragments left (covBandA), the particles go over it and the sum is
// tone mapped once - the composite's part 2 after its loop. Heavy pixels are CoverageHeavyFinish's.
// Per lane: at most COV_LIGHT loads of 12 B.
// P[0] = { entries' radiance (raw), pixel spans (raw), V's tile list (raw), tile spans (raw) }, P[1].w the colour UAV,
// P[2] = { band A radiance, resolved or UNX_NONE, edge tile mask or UNX_NONE, 0 }, P[6].xy the particle layer
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer radiance = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer pixelSpans = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer tileSpans = ResourceDescriptorHeap[P[0].w];
    const uint listed = gid.x + gid.y * 65535;
    if (listed >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform
    const uint2 span = pixelSpans.Load2(8 * (listed * COV_TILE_PIXELS + gi));
    if ((span.x >> 24) != COVC_LIGHT) return;  // no record, outside the view, or a heavy pixel
    const uint4 info = coverageTileInfo(list, listed);
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint2 pixel = uint2(info.x % tilesX, info.x / tilesX) * COV_TILE_PX + uint2(gi % COV_TILE_PX, gi / COV_TILE_PX);
    const uint first = tileSpans.Load(8 * listed) + (span.x & 0xFFFFu), count = min((span.x >> 16) & 0xFFu, COV_LIGHT);
    float3 sum = 0;
    [loop] for (uint i = 0; i < count; ++i) sum += asfloat(radiance.Load3(12 * (first + i)));
    sum += max(1 - asfloat(span.y), 0.0) * covBandA(pixel);
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].w];
    color[pixel] = shEncodeExposed(shParticles(sum, pixel, P[6].x, P[6].y));  // P[6].xy particle layer
}
