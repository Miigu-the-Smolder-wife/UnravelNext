// Coverage layer, tile-range form (INTERFACES_KO.md 7.1 v1.41; design COVERAGE_REDESIGN 4.6). Owner: V. Readers: M's
// coverage composite, S's fragment visibility. V writes it in the coverage raster (CoverageRaster.ps) and CoverageBuild.
//
// Records (16 B, CoverageFragment) in a StructuredBuffer<uint4> (ViewResources::coverageRecords). Every listed tile's
// records are one contiguous range, pixel-major inside it: listed tile j (list order) holds the elements
// [recordBase, recordBase + records), its pixel p (x + 8 y) the elements [recordBase + start(p), recordBase + start(p + 1))
// with start(p) = word j * 64 + p of ViewResources::coverageTilePixels and start(64) = records. The order of a pixel's
// records is not defined. The same vis id may appear twice in one pixel (a primitive the hardware clipped is shaded once
// per piece along the cuts, with identical values): readers sort by (depth, vis id) and keep one of equal neighbours.
// A record is opaque for the view unless its material is glass or water (alpha-tested records: their mask is the one
// after the test); a see-through record has the sign bit of its depth word set (depth is never negative).
// Per 8 x 8 pixel tile (tile = tx + ty * tilesX, tilesX = ceil(width / 8)), header (8 words): records, record base,
//   listed index + 1 (0: no records this frame), the 64-bit opaqueCovered mask (pixel p: the union of the masks of the
//   pixel's opaque records is full and the band A surface lies behind the farthest of those records, so that surface has
//   weight zero under the composite's mask-union occlusion; COVERAGE_REDESIGN 4.6), V-internal words 5..7. Words 1 and
//   5..7 of a tile without records are undefined.
// Tile list (raw buffer), header words:
//   0..2 DispatchIndirect args over the listed tiles (one group per tile; x up to 65535, then y), 3 listed tiles,
//   4 records stored, 5 blocks (a tile's records in blocks of COV_BLOCK, the last one partial), 6 record capacity,
//   7 tiles per row, 8..10 DispatchIndirect args over the blocks (one group per block), 11..15 V internal;
//   words 16 + 4 j: listed tile j = { tile, records, record base, block base } (block base: the tile's first block, an
//   exclusive prefix over the list, like the record base).
// Per-pixel depth range (ViewResources::coverageDepthRange, Texture2D<uint2> R32G32_UINT): x = the nearest record's depth
// bits (max), y = the farthest (min), see-through records included, flag cleared; (0, 0xFFFFFFFF) = no record.
// Fewer records stored than fragments appended means the pool ran out (Stats::overflow bit OVERFLOW_COVERAGE, a gate
// failure; the pool grows from the next completed frame).
//
// Every V pass over the layer does a fixed amount of work per group (COV_BLOCK records, one tile's 64 pixels, or one list
// scan of at most ceil(tiles / 1024) tiles per thread), with as many groups as the data needs (INTERFACES 3.6): no tile
// or pixel depth makes one group's work grow.
#ifndef UNX_COVERAGE_TILES_HLSLI
#define UNX_COVERAGE_TILES_HLSLI

#define COV_TILE_PX 8u
#define COV_TILE_PIXELS 64u
#define COV_BLOCK 1024u             // records per block (V's block pass; M's block stages)
#define COV_MASK_FULL 0xFFFFFFFFu
#define COV_DEPTH_SEE_THROUGH 0x80000000u  // record depth word: the material is not opaque for the view
#define COV_TILE_WORDS 8u           // header words per tile
#define COV_TILE_COUNT 0u           // records of the tile
#define COV_TILE_BASE 1u            // element of its record 0
#define COV_TILE_LISTED 2u          // listed index + 1 (0: not listed)
#define COV_TILE_OPAQUE_LO 3u       // opaqueCovered pixels 0..31
#define COV_TILE_OPAQUE_HI 4u       // opaqueCovered pixels 32..63
#define COV_LIST_ARGS 0u            // tile list header
#define COV_LIST_COUNT 3u
#define COV_LIST_RECORDS 4u
#define COV_LIST_BLOCKS 5u
#define COV_LIST_POOL 6u
#define COV_LIST_TILES_X 7u
#define COV_LIST_BLOCK_ARGS 8u
#define COV_LIST_INFO 16u           // listed tile j: words 16 + 4 j .. 16 + 4 j + 3

struct CoverageFragment  // 16 B
{
    uint visId;       // VisBuffer.hlsli packing
    uint depthBits;   // device depth (reversed Z) at the covered region's centroid, float bits | COV_DEPTH_SEE_THROUGH
    uint mask;        // 32 subsamples (Coverage.hlsli coverageSample)
    uint packed;      // octahedral normal 8 + 8 bits | area x 1023 (10 bits) << 16 | pixel in the tile (x + 8 y) << 26
};

// Strand hair records (B10, V's HairRaster.ms; INTERFACES v1.59): vis id COV_HAIR_ID | the segment's record in
// FrameResources::hairSegments (E: 2 float4 per segment; its body from FrameResources::hairBodies), the normal bits hold
// the strand coordinate u (root 0, tip 1) as unorm16; M rebuilds the tangent from p1 - p0. Every other vis id has bit 31
// clear (VisBuffer.hlsli packVisId stays below 2^31).
#define COV_HAIR_ID 0x80000000u    // top bits 10: hair, segment in bits 0..29
#define COV_STREAM_ID 0xC0000000u  // top bits 11: GPU triangle stream (v1.60), slot in bits 24..29, triangle in 0..23
bool coverageFragmentIsHair(CoverageFragment f) { return (f.visId >> 30) == 2u; }
uint coverageFragmentHairSegment(CoverageFragment f) { return f.visId & 0x3FFFFFFFu; }
// Stream records (FrameResources::triangleStreams, W): see-through, the normal bits hold the interpolated vertex normal.
bool coverageFragmentIsStream(CoverageFragment f) { return (f.visId >> 30) == 3u; }
uint coverageFragmentStreamSlot(CoverageFragment f) { return (f.visId >> 24) & 0x3Fu; }
uint coverageFragmentStreamTriangle(CoverageFragment f) { return f.visId & 0xFFFFFFu; }
float coverageFragmentHairU(CoverageFragment f) { return (f.packed & 0xFFFFu) / 65535.0; }
uint coveragePackHair(float u, float area, uint pixelInTile)
{
    const uint a = (uint)round(saturate(area) * 1023);
    return (uint)round(saturate(u) * 65535) | (a << 16) | (pixelInTile << 26);
}

uint coverageFragmentPixel(CoverageFragment f) { return f.packed >> 26; }                         // 0..63
float coverageFragmentDepth(CoverageFragment f) { return asfloat(f.depthBits & ~COV_DEPTH_SEE_THROUGH); }
bool coverageFragmentOpaque(CoverageFragment f) { return (f.depthBits & COV_DEPTH_SEE_THROUGH) == 0; }
float coverageFragmentArea(CoverageFragment f) { return ((f.packed >> 16) & 0x3FFu) / 1023.0; }  // px^2, alpha included
// Unit normal (world, interpolated at the covered region's centroid; faces the viewer for two-sided materials).
float3 coverageFragmentNormal(CoverageFragment f)
{
    const float2 e = float2(f.packed & 0xFFu, (f.packed >> 8) & 0xFFu) / 255.0 * 2 - 1;
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}

uint coverageEncodeNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.z >= 0 ? n.xy : (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    const uint2 q = uint2(round(saturate(e * 0.5 + 0.5) * 255));
    return q.x | (q.y << 8);
}

uint coveragePackFragment(float3 normal, float area, uint pixelInTile)
{
    const uint a = (uint)round(saturate(area) * 1023);
    return coverageEncodeNormal(normal) | (a << 16) | (pixelInTile << 26);
}

CoverageFragment coverageUnpackRecord(uint4 v)
{
    CoverageFragment f;
    f.visId = v.x;
    f.depthBits = v.y;
    f.mask = v.z;
    f.packed = v.w;
    return f;
}

// Readers: listed tile j = { tile, records, record base, block base }.
uint4 coverageTileInfo(ByteAddressBuffer list, uint j) { return list.Load4(4 * (COV_LIST_INFO + 4 * j)); }

// Readers: the first record of pixel p (0..64; 64 = the tile's end) of listed tile j, relative to its record base.
uint coveragePixelStart(ByteAddressBuffer tilePixels, uint4 info, uint j, uint p)
{
    return p < COV_TILE_PIXELS ? tilePixels.Load(4 * (j * COV_TILE_PIXELS + p)) : info.y;
}

// Readers: the listed tile holding block b (the last with block base <= b), by binary search over the list (at most
// log2(listed) + 1 steps; the loop is bounded by 32).
uint coverageBlockTile(ByteAddressBuffer list, uint b)
{
    uint lo = 0, hi = list.Load(4 * COV_LIST_COUNT);
    for (uint step = 0; step < 32 && hi - lo > 1; ++step)
    {
        const uint mid = (lo + hi) / 2;
        if (list.Load(4 * (COV_LIST_INFO + 4 * mid + 3)) <= b) lo = mid;
        else hi = mid;
    }
    return lo;
}

CoverageFragment coverageLoadRecord(StructuredBuffer<uint4> records, uint element) { return coverageUnpackRecord(records[element]); }

// ---- v1.40 tile-chunk names, kept only until M's composite moves to the ranges (INTERFACES 12, v1.41). They compile but
// ---- do not read the v1.41 layout: ViewResources::coverageChunkTable is invalid, which turns the v1.40 readers off.
#define COV_CHUNK_RECORDS 64u
#define COV_TILE_EXT 5u
#define COV_LIST_TABLE_SLOTS 6u
#define COV_LIST_TILES 16u
uint coverageChunkOf(ByteAddressBuffer table, StructuredBuffer<uint4> records, uint tableSlots, uint tile, uint ext, uint ordinal) { return 0; }
CoverageFragment coverageLoadRecord(StructuredBuffer<uint4> records, uint chunk, uint i)
{
    return coverageUnpackRecord(records[(chunk - 1) * COV_CHUNK_RECORDS + i % COV_CHUNK_RECORDS]);
}

#endif
