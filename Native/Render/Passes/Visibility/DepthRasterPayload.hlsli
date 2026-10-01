// Depth raster service, tile-local runs (DepthRasterRequest::tileLocal): the amplification stage's payload and the pair
// selection DepthRaster.as.hlsl and DepthRaster.ms.hlsl share. A pair is a run of consecutive set tiles of one row of
// the cluster's tile rectangle (TILE=1), a single set tile (TILE=2, atlas mode: each tile has its own atlas slot), or
// the whole rectangle when every tile of it is set (TILE=1) - the pairs tileVisit(TILE_VISIT_WRITE) wrote into a stored
// pair list before, in the same order. Nothing is stored per pair any more: the amplification group counts the pairs
// of each row and the mesh group finds its own from the per-row prefix.
// Bounds (structural, checked by the service on the CPU): a view's tile grid has at most DR_MAX_ROWS rows and
// DR_MAX_ROWS columns, so one cluster launches at most DR_MAX_ROWS^2 = 65536 mesh groups (D3D12: 2^22 per
// DispatchMesh, 65535 per dimension: a DR_GRID x DR_GRID grid), and the payload is 1,044 B (limit 16 KB).
#ifndef UNX_DEPTH_RASTER_PAYLOAD_HLSLI
#define UNX_DEPTH_RASTER_PAYLOAD_HLSLI
#include "Passes/Visibility/VisibilityCommon.hlsli"

#define DR_MAX_ROWS 256u
#define DR_GRID 256u  // mesh groups per row of the DispatchMesh grid: pair = group.x + group.y * DR_GRID

struct DepthRasterPayload
{
    uint visibleIndex;
    uint total;  // pairs of the cluster's rectangle (0: none)
    uint whole;  // 1: one pair, the whole rectangle
    uint rectLo, rectHi;  // packTileRect (inclusive tiles)
    uint prefix[DR_MAX_ROWS];  // pairs before row r of the rectangle
};

// The set tiles (single) or the runs of consecutive set tiles of row y over [x0, x1] of a view's tile mask.
uint drRowCount(ByteAddressBuffer mask, uint offset, uint tilesX, uint y, uint x0, uint x1, bool single)
{
    const uint l = y * tilesX + x0, h = y * tilesX + x1;
    uint n = 0, previous = 0;
    for (uint w = l >> 5; w <= (h >> 5); ++w)
    {
        const uint m = tileMaskBits(mask, offset, w, l, h);
        n += countbits(single ? m : m & ~((m << 1) | (previous >> 31)));
        previous = m;
    }
    return n;
}

// The k-th pair of row y over [x0, x1] (k < drRowCount): its first and last tile x (inclusive).
uint2 drRowPair(ByteAddressBuffer mask, uint offset, uint tilesX, uint y, uint x0, uint x1, bool single, uint k)
{
    const uint l = y * tilesX + x0, h = y * tilesX + x1;
    uint previous = 0;
    for (uint w = l >> 5; w <= (h >> 5); ++w)
    {
        const uint m = tileMaskBits(mask, offset, w, l, h);
        uint starts = single ? m : m & ~((m << 1) | (previous >> 31));
        const uint c = countbits(starts);
        if (k >= c)
        {
            k -= c;
            previous = m;
            continue;
        }
        for (uint j = 0; j < k; ++j) starts &= starts - 1;  // drop the k lower starts
        const uint bit = firstbitlow(starts);
        const uint start = w * 32 + bit - y * tilesX;
        if (single) return uint2(start, start);
        // the run's end: the tile before its first clear tile (tiles outside [l, h] read clear)
        uint clear = ~m & (0xFFFFFFFFu << bit);
        uint cw = w;
        while (clear == 0 && cw < (h >> 5) + 1)
        {
            ++cw;
            clear = ~tileMaskBits(mask, offset, cw, l, h);
        }
        return uint2(start, cw * 32 + firstbitlow(clear) - 1 - y * tilesX);
    }
    return uint2(x0, x0);  // (not reached for k < drRowCount)
}
#endif
