// Classification pages of the local lights (RENDERER_REDESIGN_V2 14.3-1, L3; owner A): layout shared by the pixel
// kernel (VsmClsPixel), the block pass (VsmClsBlocks), the (tile, light) classification (LocalTileClassify) and the
// readers of its result (TileLights, ShadeOpaque, the froxel integration).
//   atlas: a raw buffer of VSM_CLS_PAGE^2 x 4 B pages, page a x 6 + face of active light a at (page % pagesPerRow,
//          page / pagesPerRow) x VSM_CLS_PAGE texels; a texel holds the nearest caster's reversed-Z device depth over
//          the texel square (0 = none), from the face's projection (VsmSystem.cpp localViewProj: near = the light's
//          nearM, far = farM);
//   blocks: per page 16 x 16 floats, the maximum of its 8 x 8 texels (VsmClsBlocks);
//   tile lit records (LocalTileClassify): per 8 x 8 tile of the main view 48 B: flags (bit 0 valid), first slice, slice
//          count, then a uint2 per slice (<= 4): bit i = entry i of that slice's froxel list is lit over every pixel of
//          the tile (visibility 1: TileLights may take it FAR, ShadeOpaque needs no slot read).
#ifndef UNX_VSM_CLS_HLSLI
#define UNX_VSM_CLS_HLSLI

#define VSM_CLS_PAGE 128u
#define VSM_CLS_PAGES_PER_ROW 48u
#define VSM_CLS_TILE_BYTES 48u
#define VSM_CLS_TILE_SLICES 4u

uint2 vsmClsPageOrigin(uint page, uint pagesPerRow) { return uint2(page % pagesPerRow, page / pagesPerRow) * VSM_CLS_PAGE; }

struct VsmClsTile
{
    uint flags, firstSlice, sliceCount, pad;
    uint2 lit[4];
};
VsmClsTile vsmClsTile(uint srv, uint tileIndex)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    const uint o = tileIndex * VSM_CLS_TILE_BYTES;
    const uint4 h = b.Load4(o);
    VsmClsTile t;
    t.flags = h.x;
    t.firstSlice = h.y;
    t.sliceCount = h.z;
    t.pad = h.w;
    t.lit[0] = b.Load2(o + 16);
    t.lit[1] = b.Load2(o + 24);
    t.lit[2] = b.Load2(o + 32);
    t.lit[3] = b.Load2(o + 40);
    return t;
}
// Whether entry i of the list of froxel slice 'slice' is lit over the tile (false when the record does not apply).
bool vsmClsTileLit(VsmClsTile t, uint slice, uint i)
{
    if ((t.flags & 1u) == 0 || i >= 64) return false;
    const uint rel = slice - t.firstSlice;
    if (rel >= t.sliceCount) return false;
    return (((i < 32 ? t.lit[rel].x : t.lit[rel].y) >> (i & 31)) & 1u) != 0;
}

#endif
