// V internal: the coverage layer (band B, ARCHITECTURE 2.1; INTERFACES 7.1 v2, CoverageTiles.hlsli): root constants and
// helpers of CoverageRaster.ms/ps and CoverageBuild. Owner: V.
//
// Per frame (main view):
//  1. last frame's tiles (the tile list still holds them) get their header and chunk table cleared, so
//     the whole-screen buffers are never cleared;
//  2. band B clusters are rasterised conservatively: each pixel fragment carries the exact area of its triangle inside
//     the pixel, the 32-subsample mask, the depth at the covered region's centroid and the interpolated normal there;
//     fragments behind every band A surface around the pixel are dropped; the rest are
//     appended to their 8 x 8 tile: one count atomic per tile and wave, a chunk per 64 records taken from the pool and
//     published in the tile's chunk table (or, past its slots, the tile's extension tables) by compare-and-swap (no
//     waiting); a tile's first fragment puts it on the tile list;
//  3. the tile list's header for M: dispatch arguments over the tiles; per tile, from its records, the opaqueCovered
//     bits (the union of the opaque masks is full in front of band A) and the heavy tiles (more fragments than M's block
//     sort holds) in a list of their own.
// No sort: M's composite sorts a tile's records in groupshared.
//   P[0] cull state UAV (raw; VS_COV_* words), cull args UAV (raw), records UAV (StructuredBuffer<uint4>), chunk table
//        UAV (raw)
//   P[1] spare, tile list UAV (raw), pool chunks, chunk table slots | heavy threshold << 12
//   P[2] HiZ SRV (Texture2D<float> mip 0, UNX_NONE = none), view width, view height, HiZ mip 0 width | height << 16
//   P[3] visible SRV (uint2; CoverageBuild MODE 3: band A depth SRV), lists SRV (raw), list capacity, views SRV
//   P[4] front-face sign (+1: front = negative signed area in y-down pixels, i.e. counter-clockwise on screen;
//        -1 mirrored), tile headers UAV (raw), tiles per row, tiles
#ifndef UNX_COVERAGE_LAYER_HLSLI
#define UNX_COVERAGE_LAYER_HLSLI
#include "Passes/Visibility/VisibilityCommon.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

#define COV_STATE P[0].x
#define COV_ARGS P[0].y
#define COV_RECORDS P[0].z
#define COV_CHUNK_TABLE P[0].w
#define COV_TILE_LIST P[1].y
#define COV_CAP_CHUNKS P[1].z
#define COV_TABLE_SLOTS (P[1].w & 0xFFFu)
#define COV_HEAVY_MIN (P[1].w >> 12)
#define COV_HIZ P[2].x
#define COV_WIDTH P[2].y
#define COV_HEIGHT P[2].z
#define COV_HIZ_SIZE uint2(P[2].w & 0xFFFFu, P[2].w >> 16)
#define COV_VISIBLE P[3].x
#define COV_BAND_A_DEPTH P[3].x  // CoverageBuild MODE 3
#define COV_LISTS P[3].y
#define COV_LIST_CAPACITY P[3].z
#define COV_VIEWS P[3].w
#define COV_FRONT_SIGN asfloat(P[4].x)
#define COV_TILE_HEADERS P[4].y
#define COV_TILES_X P[4].z
#define COV_TILES P[4].w

// Primitive flags (CoverageRaster.ms -> ps).
#define COV_FLAG_ALPHA 1u   // alpha-tested material
#define COV_FLAG_QUAD 2u    // near-clipped: the polygon is the quad (a, b, c, d)
#define COV_FLAG_OPAQUE 4u  // opaque for the view (not glass or water; alpha-tested: its mask after the test);
                            // otherwise the record's depth word carries COV_DEPTH_SEE_THROUGH
#define COV_FLAG_BACK 8u    // seen from behind (two-sided): the normals are turned towards the viewer

// Vertex normals between the mesh and pixel kernels: octahedral 16 + 16 bits (the record keeps 8 + 8).
uint coverageOct32(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    const uint2 q = uint2(round(saturate(e * 0.5 + 0.5) * 65535));
    return q.x | (q.y << 16);
}

float3 coverageOct32Decode(uint v)
{
    const float2 e = float2(v & 0xFFFFu, v >> 16) / 65535.0 * 2 - 1;
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}

// Barycentric weights of point q with respect to triangle (a, b, c) (screen pixels); affine, so valid outside it.
float3 coverageBarycentric(float2 a, float2 b, float2 c, float2 q)
{
    const float2 e1 = b - a, e2 = c - a, d = q - a;
    const float det = e1.x * e2.y - e1.y * e2.x;
    if (abs(det) < 1e-20) return float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    const float s = (d.x * e2.y - d.y * e2.x) / det, t = (e1.x * d.y - e1.y * d.x) / det;
    return float3(1 - s - t, s, t);
}

#endif
