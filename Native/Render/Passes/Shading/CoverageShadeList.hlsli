// Coverage composite, compact form (shading.coverage_compact; CoverageShade.hlsli), stage S: the body of the list
// shading kernels (CoverageShadeList.hlsl: PART 1 and 2; CoverageShadeWhole.hlsl: PART 0). One group of 64 threads per
// listed tile (V's list arguments); the tile's run of entries (CoverageWalk: its fragments with weight, in pixel order)
// is shaded 64 entries at a time, one fragment per lane, so the lanes are full whatever each pixel's depth is (the last
// round of a tile is the only partial one). The fragment's pixel comes from its record (the pixel in the tile) and its
// shading is the composite's covFragmentRadiance with the same arguments:
//   PART 1  the emission, sun and local lights (COV_PART_EXPOSED); stores weight x radiance per entry
//   PART 2  the indirect light and the air's in-scattering; adds its weight x radiance to part 1's
//   PART 0  both in one kernel (the sum of the two parts up to float association): stores weight x radiance. The
//           surface, the material and the air are evaluated once per fragment instead of once per part.
// The tile's screen probes (parts 0 and 2) and its light field (g_covListed) are the group's, as in CoverageComposite.
// Per group: at most COVC_TILE_ENTRIES / 64 = 16 rounds of one shading per lane.
// P[0] = { V's records (StructuredBuffer<uint4>), tile spans (raw), V's tile list (raw), entries (raw) },
// P[1], P[3], P[4], P[5].x: the shading constants (CoverageShade.hlsli); P[2].x the entries' radiance UAV (raw, 12 B)
#define COV_PART PART
#define COV_PART_EXPOSED 1
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer tileSpans = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer entries = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer radiance = ResourceDescriptorHeap[P[2].x];
    const uint listed = gid.x + gid.y * 65535;
    if (listed >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform
    const uint2 span = tileSpans.Load2(8 * listed);  // first entry, entries
    const uint total = min(span.y, COVC_TILE_ENTRIES);
    if (total == 0) return;  // uniform: a tile of heavy pixels, or of fragments behind band A
    const uint4 info = coverageTileInfo(list, listed);  // tile, records, record base, block base
    g_covListed = listed;  // L2c: the records' tile field (CoverageShade.hlsli)
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint2 tileCoord = uint2(info.x % tilesX, info.x / tilesX);
#if PART != 1
    const uint4 probeRecord = covProbeFetch(tileCoord, gi);  // (the indirect light is part 2's, or the whole kernel's)
    if (P[4].w != UNX_NONE && (P[3].z & 6) != 6) giProbeTileStore(gi, probeRecord);
    GroupMemoryBarrierWithGroupSync();
#endif
    [loop] for (uint i = gi; i < total; i += 64)
    {
        const uint2 entry = entries.Load2(8 * (span.x + i));  // element, weight
        const CoverageFragment fr = coverageUnpackRecord(records[entry.x]);
        const uint p = coverageFragmentPixel(fr);
        const uint2 pixel = tileCoord * COV_TILE_PX + uint2(p % COV_TILE_PX, p / COV_TILE_PX);
        const float3 value = asfloat(entry.y) * covFragmentRadiance(fr.visId, entry.x, pixel, P[3].z);
#if PART != 2
        radiance.Store3(12 * (span.x + i), asuint(value));
#else
        radiance.Store3(12 * (span.x + i), asuint(asfloat(radiance.Load3(12 * (span.x + i))) + value));
#endif
    }
}
