// V internal: depth buckets of the coverage raster (visibility.coverage_depth_buckets; COVERAGE_REDESIGN 4.5 "depth batch
// raster + union update between the batches", the reference's raster bins and depth buckets). Owner: V.
//
// A stored fragment that lies behind a full union of nearer opaque fragments of its pixel has weight zero in M's
// composite (mask-union occlusion, nearer first). The band B list is therefore drawn nearer clusters first, in buckets:
//   bins     (CoverageBins.hlsl) every band B list entry gets a fine depth bin from its cluster sphere's nearest
//            distance (entries behind band A - the final HiZ over the sphere's rectangle - are left out); a prefix over
//            the bins sorts the entries nearer first and cuts the sorted list into buckets of equal entry counts;
//   raster   one pass per bucket (CoverageRaster.ms / ps, optionally CoverageRasterSw.hlsl before it);
//   cover    after each bucket but the last, the bucket's stored opaque fragments enter the per-pixel cover
//            { U, ~D } (U: the union of their masks, D: the farthest of them; one word pair per key tile x 64 + pixel)
//            and their tile's header words COV_TILE_FULL (pixels whose U is full) and COV_TILE_NEAR (~ the farthest
//            opaque fragment of the tile).
// A later bucket drops a fragment with U full and depth < D (pixel kernel), a triangle or a cluster whose rectangle lies
// in tiles that are full in every pixel and whose nearest depth is behind every such tile's farthest fragment (mesh
// kernel; the compute rasteriser the same).
// Exact: the contributors of U are stored fragments nearer than D, so every one of them is composited before a
// fragment behind D and their union leaves it no subsample and no share (CoverageComposite.hlsl: seen = 0); U and D only
// hold fragments of buckets drawn before, read after their cover pass ended, so the set of stored fragments does not
// depend on the order the GPU ran anything in. D is the farthest of every stored opaque fragment of the pixel: once U
// is full only fragments nearer than D are stored, so D stays.
// What changes for readers: the hidden fragments are no longer in the records (fewer records per pixel; the pixel's
// farthest record, coverageDepthRange.y, is the farthest stored one).
//
// Root constants of the raster kernels beside CoverageLayer.hlsli's (slots its raster passes leave unused):
//   P[0].y bins SRV (raw; CoverageRasterSw: UAV), UNX_NONE: the band B list is drawn whole (no buckets)
//   P[0].z cover SRV (raw), UNX_NONE: no fragment is cut (the first bucket, the other producers' passes)
//   P[1].y tile headers SRV (raw), UNX_NONE: no tile test
//   P[1].w the bucket drawn (bits 0..7) and the raster's switches (COV_RASTER_*; the hair and stream passes: 0)
// CoverageBins.hlsl: P[0].y bins UAV, P[0].z cover UAV, P[1].y bin arguments UAV (raw), P[1].w the bucket (the cover
// arguments' pass), P[4].y tile headers UAV, P[6].x buckets of the frame.
#ifndef UNX_COVERAGE_BUCKETS_HLSLI
#define UNX_COVERAGE_BUCKETS_HLSLI
#include "Passes/Visibility/CoverageLayer.hlsli"

#define COV_BINS P[0].y
#define COV_COVER P[0].z
#define COV_TILE_HIZ P[1].y
#define COV_BIN_ARGS P[1].y  // CoverageBins.hlsl
#define COV_BUCKET P[1].w
#define COV_BUCKETS P[6].x   // CoverageBins.hlsl
#define COV_RASTER_BUCKET (COV_BUCKET & 0xFFu)
#define COV_RASTER_TRIANGLE_CULL ((COV_BUCKET & 0x100u) != 0)    // visibility.coverage_triangle_cull (mesh kernel, compute rasteriser)
#define COV_RASTER_COMPUTE ((COV_BUCKET & 0x200u) != 0)          // the compute rasteriser ran over this bucket: its triangles are skipped
#define COV_RASTER_DROP_WEIGHTLESS ((COV_BUCKET & 0x400u) != 0)  // visibility.coverage_drop_weightless (fragment kernels)

// Tile header words of the cover passes (V internal, CoverageTiles.hlsli words 5..7; zero in a tile without stored
// opaque fragments: the tile clear empties them with the header).
#define COV_TILE_FULL 6u  // pixels of the tile whose cover union is full
#define COV_TILE_NEAR 7u  // ~ (depth bits of the farthest stored opaque fragment the cover passes saw in the tile)

// The binned list (raw words). Per band B list entry e < capacity (COV_LIST_CAPACITY): word COVB_HEADER + e its fine
// bin (COVB_NO_BIN: not drawn), word COVB_HEADER + capacity + i sorted entry i (a list entry, nearer bins first), words
// COVB_HEADER + 2 capacity + 4 i .. + 3 the triangles of sorted entry i the compute rasteriser took (bit per triangle).
#define COVB_ENTRIES 0u      // sorted entries (the list's entries that are not behind band A)
#define COVB_COVER_END 1u    // stream entries the cover passes have taken
#define COVB_COVER_BEGIN 2u  // first stream entry of the cover pass being run
#define COVB_FIRST 8u        // + bucket: its first sorted entry
#define COVB_COUNT 16u       // + bucket: its entries
#define COVB_HISTOGRAM 64u   // + fine bin: entries of the bin; from the prefix pass on, the bin's write cursor
#define COVB_FINE 256u       // fine bins: 12.8 per octave of distance over [COVB_NEAR, COVB_NEAR x 2^20] metres
#define COVB_NEAR 0.0625     // m
#define COVB_OCTAVES 20.0
#define COVB_HEADER 320u
#define COVB_BUCKETS_MAX 8u
#define COVB_NO_BIN 0xFFFFFFFFu
uint covbBinOf(uint capacity, uint entry) { return COVB_HEADER + entry; }
uint covbSorted(uint capacity, uint i) { return COVB_HEADER + capacity + i; }
uint covbTaken(uint capacity, uint i) { return COVB_HEADER + 2 * capacity + 4 * i; }
// Dispatch arguments of the bucket passes (raw words, a buffer of their own: the bins are written by passes that take
// their group count from here).
#define COVB_ARG_ENTRIES 0u  // 0..2 over the band B list, 64 entries per group
#define COVB_ARG_COVER 3u    // 3..5 over the stream entries of the bucket just drawn, COV_BLOCK per group
#define COVB_ARG_DRAW 8u     // + 3 x bucket: one group per sorted entry of the bucket (mesh and compute raster)
#define COVB_ARG_WORDS 32u

// Fine bin of a distance from the eye (metres): log2 spaced, nearer = smaller.
uint covbFineBin(float distance)
{
    const float t = log2(max(distance, COVB_NEAR) / COVB_NEAR) * (COVB_FINE / COVB_OCTAVES);
    return (uint)clamp(t, 0.0, COVB_FINE - 1.0);
}

// Tiles one rectangle test loads at most per axis: a larger rectangle is not tested (kept).
#define COV_TILE_SPAN_TRIANGLE 3u
#define COV_TILE_SPAN_CLUSTER 6u

// True when every fragment in the pixel rectangle [lo, hi] (render-target pixels) no nearer than 'nearest' (device depth,
// reversed Z) is one the pixel kernel drops behind band A (coverageAboveBandA): the farthest band A depth over the
// rectangle grown by that test's one-pixel neighbourhood is nearer than 'nearest'. One HiZ level where the grown
// rectangle spans at most 2 x 2 texels (4 loads).
bool coverageRectBehindBandA(float2 lo, float2 hi, float nearest)
{
    if (COV_HIZ == UNX_NONE) return false;
    const int2 view = int2(COV_WIDTH, COV_HEIGHT);
    const int2 p0 = max(int2(floor(lo)) - 1, 0), p1 = min(int2(floor(hi)) + 1, view - 1);
    if (any(p1 < p0)) return false;
    Texture2D<float> hiz = ResourceDescriptorHeap[COV_HIZ];
    uint w, h, levels;
    hiz.GetDimensions(0, w, h, levels);
    // texels of mip m are 2^(m+1) pixels wide: pixels p0..p1 lie in at most two of them per axis when p1 - p0 <= 2^(m+1)
    const uint extent = (uint)max(max(p1.x - p0.x, p1.y - p0.y), 1);
    const uint mip = min((uint)max(0.0, ceil(log2((float)extent)) - 1.0), levels - 1);
    const uint shift = mip + 1;
    if (extent > (1u << shift)) return false;  // (the chain's last level is finer than the rectangle needs: no answer)
    const uint2 a = uint2(p0) >> shift, b = uint2(p1) >> shift;
    const uint2 size = (COV_HIZ_SIZE + (1u << mip) - 1) >> mip;  // mip sizes round up (HiZ.hlsl)
    float farthest = 1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 t = min(uint2((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y), size - 1);
        farthest = min(farthest, hiz.Load(int3(t, mip)));
    }
    return nearest < farthest;
}

// True when every pixel of the view under the rectangle [lo, hi] has a full cover union and 'nearest' lies behind the
// farthest stored opaque fragment of every tile under it: every fragment there no nearer than 'nearest' is one the
// pixel kernel drops behind the cover. At most span x span tile headers (8 B each).
bool coverageRectBehindCover(float2 lo, float2 hi, float nearest, uint span)
{
    if (COV_TILE_HIZ == UNX_NONE) return false;
    const int2 view = int2(COV_WIDTH, COV_HEIGHT);
    const int2 p0 = max(int2(floor(lo)), 0), p1 = min(int2(floor(hi)), view - 1);
    if (any(p1 < p0)) return false;
    const uint2 t0 = uint2(p0) / COV_TILE_PX, t1 = uint2(p1) / COV_TILE_PX;
    if (t1.x - t0.x >= span || t1.y - t0.y >= span) return false;
    ByteAddressBuffer tiles = ResourceDescriptorHeap[COV_TILE_HIZ];
    const uint depthBits = asuint(nearest);  // non-negative floats order like their bits; a negative one never passes
    bool behind = true;
    for (uint y = t0.y; y <= t1.y && behind; ++y)
        for (uint x = t0.x; x <= t1.x && behind; ++x)
        {
            const uint2 words = tiles.Load2(4 * ((y * COV_TILES_X + x) * COV_TILE_WORDS + COV_TILE_FULL));
            const uint2 inside = min(uint2(view) - uint2(x, y) * COV_TILE_PX, COV_TILE_PX);  // the tile's pixels in the view
            behind = words.x == inside.x * inside.y && depthBits < ~words.y;
        }
    return behind;
}

// The same answer pixel by pixel, for a rectangle of at most COV_COVER_PIXELS pixels of the view (a small triangle: most
// of band B): every pixel under it has a full cover union and 'nearest' lies behind that pixel's farthest contributor.
// A covered pixel among uncovered ones of its tile still hides what lies only on it. One 8 B load per pixel.
#define COV_COVER_PIXELS 8u
bool coverageRectBehindCoverPixels(float2 lo, float2 hi, float nearest)
{
    if (COV_COVER == UNX_NONE) return false;
    const int2 view = int2(COV_WIDTH, COV_HEIGHT);
    const int2 p0 = max(int2(floor(lo)), 0), p1 = min(int2(floor(hi)), view - 1);
    if (any(p1 < p0)) return false;
    const uint2 size = uint2(p1 - p0 + 1);
    if (size.x * size.y > COV_COVER_PIXELS) return false;
    ByteAddressBuffer cover = ResourceDescriptorHeap[COV_COVER];
    const uint depthBits = asuint(nearest);
    bool behind = true;
    for (uint k = 0; k < COV_COVER_PIXELS && k < size.x * size.y && behind; ++k)
    {
        const uint2 pixel = uint2(p0) + uint2(k % size.x, k / size.x);
        const uint key = ((pixel.y / COV_TILE_PX) * COV_TILES_X + pixel.x / COV_TILE_PX) * COV_TILE_PIXELS + (pixel.x % COV_TILE_PX) + COV_TILE_PX * (pixel.y % COV_TILE_PX);
        const uint2 c = cover.Load2(8 * key);
        behind = c.x == COV_MASK_FULL && depthBits < ~c.y;
    }
    return behind;
}

// A cluster's world sphere against the tiles' cover (the group-uniform test of the mesh and compute raster kernels).
// A sphere whose box reaches the near plane is never hidden.
bool coverageSphereBehindCover(CullView v, float4 s)
{
    if (COV_TILE_HIZ == UNX_NONE) return false;
    float4 rect;
    float nearest;
    if (!projectSphere(v.viewProj, v.viewportSize, s, rect, nearest)) return false;
    return coverageRectBehindCover(rect.xy, rect.zw, nearest, COV_TILE_SPAN_CLUSTER);
}

#endif
