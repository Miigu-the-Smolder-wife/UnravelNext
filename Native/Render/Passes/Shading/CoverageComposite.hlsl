// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1 AREA=0,1
// Coverage composite, stage E (CoverageShade.hlsli; design COVERAGE_REDESIGN 4.5): one group of 64 threads per listed
// tile (V's list arguments), one pixel per lane, reading the pixel's range of V's records (pixel-major, v1.41). A light
// pixel (1..COV_LIGHT fragments) loads its depth keys, sorts them with an unrolled bitonic network in registers (nearer
// first) and composites front to back (the E composite's rule: area x the share of its mask no nearer fragment covered,
// capped by what they left; only fragments with a visible share are shaded) until the mask union is full or the band A
// surface is reached; the band A surface takes the rest (covBandA), and the sum is tone mapped once. A heavy pixel (more
// fragments) becomes a heavy record { pixel, first record, count, runs of COV_BLOCK, first cursor slot, state } for
// CoverageHeavy* (the most runs of one pixel go to the state: the run sort's z argument). At most COV_LIGHT key loads and
// shadings per lane: the group's work is bounded whatever the tile holds.
// Fragment shading (covShadeFragment): the triangle through the covered region's centroid with the material resolve's
// surface, then the band A kernel's lighting. Shadows: S's fragment visibility (4.3) once published (P[3].w).
// P[0] = { V's records (StructuredBuffer<uint4>), V's tile pixels (raw), V's tile list (raw), state UAV (raw) }
// P[1], P[3], P[4], P[5].x: the shading constants (CoverageShade.hlsli); P[2] = { band A radiance, resolved or UNX_NONE,
// edge tile mask or UNX_NONE, heavy records UAV (raw) }, P[5] = { .., heavy capacity (records), cursor capacity, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

groupshared uint2 gs_sorted[COV_TILE_PIXELS][COV_LIGHT];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer tilePixels = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    const uint listed = gid.x + gid.y * 65535;
    if (listed >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform
    const uint4 info = coverageTileInfo(list, listed);  // tile, records, record base, block base
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint2 tileCoord = uint2(info.x % tilesX, info.x / tilesX);
    const uint4 probeRecord = covProbeFetch(tileCoord, gi);
    const uint first = info.z + coveragePixelStart(tilePixels, info, listed, gi);
    const uint count = info.z + coveragePixelStart(tilePixels, info, listed, gi + 1) - first;
    if (P[4].w != UNX_NONE && (P[3].z & 6) != 6) giProbeTileStore(gi, probeRecord);
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = tileCoord * COV_TILE_PX + uint2(gi % COV_TILE_PX, gi / COV_TILE_PX);
    if (count == 0 || any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    if (count > COV_LIGHT)
    {
        RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
        const uint runs = (count + COV_BLOCK - 1) / COV_BLOCK;
        uint h, cursor;
        state.InterlockedAdd(4 * COVS_HEAVY, 1, h);
        state.InterlockedAdd(4 * COVS_RUNS, runs, cursor);
        if (h >= P[5].y || cursor + runs > P[5].z)
        {
            state.InterlockedOr(4 * COVS_ERRORS, COV_M_ERROR_ITERATION_LIMIT);  // capacities sized from the pool: a defect
            return;
        }
        state.InterlockedMax(4 * COVS_MAX_RUNS, runs);
        RWByteAddressBuffer heavy = ResourceDescriptorHeap[P[2].w];
        const uint base = 4 * h * COVH_WORDS;
        heavy.Store4(base, uint4(pixel.x | (pixel.y << 16), first, count, runs));
        heavy.Store4(base + 16, uint4(cursor, 0, asuint(0.0), 0));
        heavy.Store4(base + 32, uint4(0, 0, 0, 0xFFFFFFFFu));  // sum, last key: none taken yet
        heavy.Store4(base + 48, uint4(0, 0, 0, 0));
        return;
    }

    // Sort in registers (constant indices only).
    uint2 a[COV_LIGHT];
    [unroll] for (uint i = 0; i < COV_LIGHT; ++i) a[i] = i < count ? covKey(records, first + i) : COV_KEY_AFTER_ALL;
    [unroll] for (uint size = 2; size <= COV_LIGHT; size <<= 1)
        [unroll] for (uint stride = size / 2; stride > 0; stride >>= 1)
            [unroll] for (uint k = 0; k < COV_LIGHT; ++k)
            {
                const uint j = k ^ stride;
                if (j <= k) continue;
                const bool up = (k & size) == 0;
                if (up ? covBefore(a[j], a[k]) : covBefore(a[k], a[j]))
                {
                    const uint2 t = a[k];
                    a[k] = a[j];
                    a[j] = t;
                }
            }
    [unroll] for (uint s = 0; s < COV_LIGHT; ++s) gs_sorted[gi][s] = a[s];

    Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].z];
    const uint bandA = asuint(bandDepth[pixel]);  // non-negative floats order like their bits
    float3 sum = 0;
    float used = 0;
    uint covered = 0, prevVisId = 0, prevDepth = 0;
    [loop] for (uint f = 0; f < count; ++f)
    {
        const uint2 key = gs_sorted[gi][f];
        if (key.x < bandA) break;  // the rest lie behind the band A surface
        const CoverageFragment fr = coverageUnpackRecord(records[key.y]);
        if (fr.visId == prevVisId && key.x == prevDepth) continue;  // the hardware's clip duplicate
        prevVisId = fr.visId;
        prevDepth = key.x;
        const uint bits = countbits(fr.mask);
        const float seen = bits > 0 ? countbits(fr.mask & ~covered) / (float)bits : 1 - countbits(covered) / 32.0;
        const float w = min(coverageFragmentArea(fr) * seen, max(1 - used, 0.0));
        if (w > 0) sum += w * covShadeFragment(fr.visId, key.y, pixel, P[3].z);
        used += w;
        covered |= fr.mask;
        if (covered == COV_MASK_FULL || used >= 1) break;
    }
    sum += max(1 - used, 0.0) * covBandA(pixel);
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].w];
    color[pixel] = shEncodeExposed(shParticles(sum, pixel, P[6].x, P[6].y));  // P[6].xy particle layer
}
