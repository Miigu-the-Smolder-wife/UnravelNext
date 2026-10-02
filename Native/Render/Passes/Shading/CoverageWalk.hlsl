// unx-kernel: cs_6_6 main
// Coverage composite, compact form (shading.coverage_compact; CoverageShade.hlsli), stage W: one group of 64 threads per
// listed tile (V's list arguments), one pixel per lane, reading the pixel's range of V's records. A light pixel
// (1..COV_LIGHT fragments) sorts its depth keys in registers (nearer first) and walks them front to back exactly as
// CoverageComposite does - band A ends the walk, the hardware's clip duplicate is skipped, the weight is the area x the
// share of the mask no nearer fragment covered, capped by what they left, until the mask union is full or the weights
// reach 1 - but shades nothing: each fragment with weight becomes an entry { element, weight }. The tile's entries are
// one run of the entry list in pixel order (a prefix over the 64 lanes, one atomic per tile), so the shading kernels
// (CoverageShadeList) fill their lanes whatever each pixel's depth is. A heavy pixel becomes a heavy record for
// CoverageHeavy*, as in the composite's part 1.
// Per group: COV_LIGHT key loads, the sort network and at most COV_LIGHT walk steps per lane, a 6-step prefix.
// P[0] = { V's records (StructuredBuffer<uint4>), V's tile pixels (raw), V's tile list (raw), state UAV (raw) },
// P[1] = { visible clusters, 0, band A depth, 0 }, P[2] = { entries UAV (raw), pixel spans UAV (raw), tile spans UAV
// (raw), heavy records UAV (raw) }, P[5] = { 0, heavy capacity (records), cursor capacity, entry capacity }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

groupshared uint2 gs_sorted[COV_TILE_PIXELS][COV_LIGHT];  // the lane's sorted keys, then its entries in place
groupshared uint gs_scan[COV_TILE_PIXELS];
groupshared uint gs_base;

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer tilePixels = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    const uint listed = gid.x + gid.y * 65535;
    if (listed >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform
    const uint4 info = coverageTileInfo(list, listed);  // tile, records, record base, block base
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint2 tileCoord = uint2(info.x % tilesX, info.x / tilesX);
    const uint first = info.z + coveragePixelStart(tilePixels, info, listed, gi);
    const uint count = info.z + coveragePixelStart(tilePixels, info, listed, gi + 1) - first;
    const uint2 pixel = tileCoord * COV_TILE_PX + uint2(gi % COV_TILE_PX, gi / COV_TILE_PX);
    uint kind = COVC_NONE, n = 0, walked = 0;
    float used = 0;
    if (count > COV_LIGHT && all(pixel < uint2(g_viewWidth, g_viewHeight)))
    {
        kind = COVC_HEAVY;
        const uint runs = (count + COV_BLOCK - 1) / COV_BLOCK;
        uint h, cursor;
        state.InterlockedAdd(4 * COVS_HEAVY, 1, h);
        state.InterlockedAdd(4 * COVS_RUNS, runs, cursor);
        if (h >= P[5].y || cursor + runs > P[5].z) state.InterlockedOr(4 * COVS_ERRORS, COV_M_ERROR_ITERATION_LIMIT);  // capacities sized from the pool: a defect
        else
        {
            state.InterlockedMax(4 * COVS_MAX_RUNS, runs);
            RWByteAddressBuffer heavy = ResourceDescriptorHeap[P[2].w];
            const uint base = 4 * h * COVH_WORDS;
            heavy.Store4(base, uint4(pixel.x | (pixel.y << 16), first, count, runs));
            heavy.Store4(base + 16, uint4(cursor, 0, asuint(0.0), 0));
            heavy.Store4(base + 32, uint4(0, 0, 0, 0xFFFFFFFFu));  // sum, last key: none taken yet
            heavy.Store4(base + 48, uint4(0, 0, 0, 0));
        }
    }
    else if (count > 0 && all(pixel < uint2(g_viewWidth, g_viewHeight)))
    {
        kind = COVC_LIGHT;
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
                    if (up ? covBefore(a[j], a[k], records, P[1].x) : covBefore(a[k], a[j], records, P[1].x))
                    {
                        const uint2 t = a[k];
                        a[k] = a[j];
                        a[j] = t;
                    }
                }
        [unroll] for (uint s = 0; s < COV_LIGHT; ++s) gs_sorted[gi][s] = a[s];

        Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].z];
        const uint bandA = asuint(bandDepth[pixel]);  // non-negative floats order like their bits
        uint covered = 0, prevVisId = 0, prevDepth = 0;
        [loop] for (uint f = 0; f < count; ++f)
        {
            const uint2 key = gs_sorted[gi][f];
            if (key.x < bandA) break;  // the rest lie behind the band A surface
            ++walked;
            const CoverageFragment fr = coverageUnpackRecord(records[key.y]);
            if (fr.visId == prevVisId && key.x == prevDepth) continue;  // the hardware's clip duplicate
            prevVisId = fr.visId;
            prevDepth = key.x;
            const uint bits = countbits(fr.mask);
            const float seen = bits > 0 ? countbits(fr.mask & ~covered) / (float)bits : 1 - countbits(covered) / 32.0;
            const float w = min(coverageFragmentArea(fr) * seen, max(1 - used, 0.0));
            if (w > 0)
            {
                gs_sorted[gi][n] = uint2(key.y, asuint(w));  // (n <= f: the keys before f were read)
                ++n;
            }
            used += w;
            covered |= fr.mask;
            if (covered == COV_MASK_FULL || used >= 1) break;
        }
    }

    // The tile's run of entries: the lanes' counts, their exclusive prefix, one atomic for the tile.
    gs_scan[gi] = n;
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 1; o < COV_TILE_PIXELS; o <<= 1)  // inclusive scan (6 steps)
    {
        const uint v = gs_scan[gi] + (gi >= o ? gs_scan[gi - o] : 0);
        GroupMemoryBarrierWithGroupSync();
        gs_scan[gi] = v;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint offset = gs_scan[gi] - n, total = gs_scan[COV_TILE_PIXELS - 1];
    if (gi == 0)
    {
        uint allocated = 0;
        if (total > 0) state.InterlockedAdd(4 * COVS_ENTRIES, total, allocated);
        gs_base = allocated;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint base = gs_base;
    // (every record gives at most one entry and the capacity is V's record capacity: a defect when it does not fit)
    const bool fits = base + total <= P[5].w && total <= COVC_TILE_ENTRIES;
    if (!fits && gi == 0) state.InterlockedOr(4 * COVS_ERRORS, COV_M_ERROR_ITERATION_LIMIT);
    RWByteAddressBuffer entries = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer pixelSpans = ResourceDescriptorHeap[P[2].y];
    RWByteAddressBuffer tileSpans = ResourceDescriptorHeap[P[2].z];
    const uint stored = fits ? min(n, COV_LIGHT) : 0u;
    for (uint e = 0; e < stored; ++e) entries.Store2(8 * (base + offset + e), gs_sorted[gi][e]);
    pixelSpans.Store2(8 * (listed * COV_TILE_PIXELS + gi), uint2(offset | (stored << 16) | (kind << 24), asuint(used)));
    if (gi == 0) tileSpans.Store2(8 * listed, uint2(base, fits ? total : 0u));

    // Statistics: the light pixels, the records their walks visited, the records with weight.
    const uint walkedSum = WaveActiveSum(walked), shadedSum = WaveActiveSum(n), lightSum = WaveActiveCountBits(kind == COVC_LIGHT);
    if (WaveIsFirstLane())
    {
        if (walkedSum > 0) state.InterlockedAdd(4 * COVS_WALKED, walkedSum);
        if (shadedSum > 0) state.InterlockedAdd(4 * COVS_SHADED, shadedSum);
        if (lightSum > 0) state.InterlockedAdd(4 * COVS_LIGHT_PIXELS, lightSum);
    }
}
