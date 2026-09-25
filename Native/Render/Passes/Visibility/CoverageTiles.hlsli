// Coverage layer, tile-chunk form (INTERFACES_KO.md 7.1 v2; request 20260926_V_coverage_layer_v2.md, design
// COVERAGE_REDESIGN 4.5). Owner: V. Readers: M's coverage composite, S's fragment visibility. V writes it in the coverage
// raster (CoverageRaster.ps) and CoverageBuild.
//
// Per 8 x 8 pixel tile (tile = tx + ty * tilesX, tilesX = ceil(width / 8)):
//   header (8 words): fragment count, nearest and farthest fragment depth (reversed-Z device depth as float bits:
//          zNear = max, zFar = min), the 64-bit opaqueCovered mask (pixel p = x + 8 y: a fragment covering the whole
//          pixel with an opaque material, so every band A surface behind it has weight zero), the first extension chunk
//          table, spare
//   chunk table: tableSlots words (list header word 6), word c = chunk index + 1 of the tile's c-th chunk (0 = none)
//   extension tables: the tile's chunks from ordinal tableSlots on. An extension table is one chunk of the record pool
//          read as 256 words: words 0..254 the next 255 ordinals' chunks, word 255 the next extension table (chunk
//          index + 1). The chain has no length limit: a tile holds any number of fragments.
// Records (16 B, CoverageFragment) in chunks of 64 in a raw buffer: the tile's i-th fragment is record i % 64 of its
// chunk i / 64 (byte (chunk index) * 1024 + (i % 64) * 16). Records of a tile are in append order (not sorted); the same
// vis id may appear twice in one pixel (a primitive the hardware clipped is shaded once per piece along the cuts, with
// identical values): readers sort by (pixel, depth, vis id) and keep one of equal neighbours. A chunk slot of 0 below
// the tile's count means the pool ran out (Stats::overflow bit OVERFLOW_COVERAGE, a gate failure).
// bDepth (R32_UINT per pixel): the nearest depth (float bits, atomic max) of a fragment covering the whole pixel with an
// opaque material; the raster drops fragments behind it (their weight is zero).
// Tile list (raw buffer), header words:
//   0..2 DispatchIndirect args over the listed tiles (one group per tile; x up to 65535, then y), 3 tile count,
//   4 fragments appended, 5 chunks allocated, 6 chunk table slots per tile, 7 tiles per row,
//   8..10 DispatchIndirect args over the heavy tiles (one group per tile), 11 heavy tile count, 12 heavy threshold (a
//   heavy tile holds more fragments than this: M's block sort), 13 first word of the heavy tile list, 14..15 spare;
//   words 16.. the tile indices of tiles with fragments, words [word 13] .. the heavy tiles' indices (a subset).
#ifndef UNX_COVERAGE_TILES_HLSLI
#define UNX_COVERAGE_TILES_HLSLI

#define COV_TILE_PX 8u
#define COV_CHUNK_RECORDS 64u
#define COV_CHUNK_BYTES 1024u
#define COV_EXT_SLOTS 255u        // chunk slots per extension table (word 255 links the next table)
#define COV_TILE_WORDS 8u         // header words per tile
#define COV_TILE_COUNT 0u         // fragments of the tile
#define COV_TILE_ZNEAR 1u         // nearest depth (float bits; 0 = none)
#define COV_TILE_ZFAR 2u          // farthest depth (float bits; 0xFFFFFFFF = none)
#define COV_TILE_OPAQUE_LO 3u     // opaqueCovered pixels 0..31
#define COV_TILE_OPAQUE_HI 4u     // opaqueCovered pixels 32..63
#define COV_TILE_EXT 5u           // first extension chunk table (chunk index + 1; 0 = none)
#define COV_LIST_ARGS 0u          // tile list header
#define COV_LIST_COUNT 3u
#define COV_LIST_FRAGMENTS 4u
#define COV_LIST_CHUNKS 5u
#define COV_LIST_TABLE_SLOTS 6u
#define COV_LIST_TILES_X 7u
#define COV_LIST_HEAVY_ARGS 8u
#define COV_LIST_HEAVY_COUNT 11u
#define COV_LIST_HEAVY_MIN 12u
#define COV_LIST_HEAVY_START 13u
#define COV_LIST_TILES 16u        // first tile index

struct CoverageFragment  // 16 B
{
    uint visId;       // VisBuffer.hlsli packing
    float depth;      // device depth (reversed Z) at the covered region's centroid
    uint mask;        // 32 subsamples (Coverage.hlsli coverageSample)
    uint packed;      // octahedral normal 8 + 8 bits | area x 1023 (10 bits) << 16 | pixel in the tile (x + 8 y) << 26
};

uint coverageFragmentPixel(CoverageFragment f) { return f.packed >> 26; }                         // 0..63
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
    f.depth = asfloat(v.y);
    f.mask = v.z;
    f.packed = v.w;
    return f;
}

// Readers (after V's passes): the tile's ordinal-th chunk (index + 1; 0 = none). 'ext' is header word COV_TILE_EXT.
uint coverageChunkOf(ByteAddressBuffer table, ByteAddressBuffer records, uint tableSlots, uint tile, uint ext, uint ordinal)
{
    if (ordinal < tableSlots) return table.Load(4 * (tile * tableSlots + ordinal));
    uint e = ordinal - tableSlots, t = ext;
    while (t != 0 && e >= COV_EXT_SLOTS)
    {
        t = records.Load((t - 1) * COV_CHUNK_BYTES + 4 * COV_EXT_SLOTS);
        e -= COV_EXT_SLOTS;
    }
    return t == 0 ? 0 : records.Load((t - 1) * COV_CHUNK_BYTES + 4 * e);
}

// Readers: record i of a tile whose i / 64-th chunk is 'chunk' (non-zero).
CoverageFragment coverageLoadRecord(ByteAddressBuffer records, uint chunk, uint i)
{
    return coverageUnpackRecord(records.Load4((chunk - 1) * COV_CHUNK_BYTES + 16 * (i % COV_CHUNK_RECORDS)));
}

#endif
