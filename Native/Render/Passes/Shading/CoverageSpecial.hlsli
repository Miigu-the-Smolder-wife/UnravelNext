// Pre-shaded coverage records (INTERFACES 7.1 v1.75). Owner: M. Readers and writers: M's coverage composite and
// CoverageSpecial.hlsl, W's water record pass (tracks::water), later the hair owner.
// ViewResources::coverageSpecial lists the special records of the view's coverage layer (V, v1.73: { element, kind });
// their owners shade them into ViewResources::coverageRecordRadiance before the composite, which only reads the value
// (CoverageShade.hlsli covFragmentRadiance) and weights it like any record: area x the share its mask leaves uncovered,
// up to full coverage, band A behind only in the remainder.
// Entry: 8 B per coverageRecords element, uint2 { f16 r | f16 g << 16, f16 b }: exposed linear radiance with the aerial
// perspective in front of the record, (L x T_air + L_inscatter) x g_exposure, the unit of covShadeFragment.
#ifndef UNX_M_COVERAGE_SPECIAL_HLSLI
#define UNX_M_COVERAGE_SPECIAL_HLSLI

#include "Passes/Visibility/CoverageTiles.hlsli"

uint2 covPackRadiance(float3 e)
{
    const float3 c = min(max(e, 0.0), 65504.0);  // (f16's largest finite value: a brighter record saturates, never inf)
    return uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b));
}

float3 covUnpackRadiance(uint2 v) { return float3(f16tof32(v.x & 0xFFFFu), f16tof32(v.x >> 16), f16tof32(v.y & 0xFFFFu)); }

// The pixel of a coverage record: its listed tile by binary search of the tile list's record bases (exclusive prefixes
// in list order), the pixel in the tile from the record's packed bits. At most 32 steps (listed tiles < 2^32).
uint2 covRecordPixel(ByteAddressBuffer list, uint element, CoverageFragment f)
{
    uint lo = 0, hi = list.Load(4 * COV_LIST_COUNT);  // the tile holding element is in [lo, hi)
    [loop] for (uint step = 0; step < 32 && hi - lo > 1; ++step)
    {
        const uint mid = (lo + hi) / 2;
        if (coverageTileInfo(list, mid).z <= element) lo = mid;
        else hi = mid;
    }
    const uint tile = coverageTileInfo(list, lo).x, tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint p = f.packed >> 26;
    return uint2(tile % tilesX, tile / tilesX) * COV_TILE_PX + uint2(p % COV_TILE_PX, p / COV_TILE_PX);
}

#endif
