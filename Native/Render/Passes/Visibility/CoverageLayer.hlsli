// V internal: the coverage layer (band B, ARCHITECTURE 2.1; INTERFACES 7.1): records, root constants and helpers of
// CoverageRaster.ms/ps and CoverageBuild. Owner: V.
//
// Per frame (main view):
//  1. the heads of last frame's coverage pixels are cleared (the pixel list still holds them), so the whole-screen
//     heads are never cleared;
//  2. band B clusters are rasterised conservatively: each pixel fragment carries the exact area of its triangle inside
//     the pixel, the 32-subsample mask and the depth at the covered region's centroid, and is appended (raw, 20 B) to a
//     per-pixel linked list (heads = newest fragment + 1) unless every band A surface around the pixel is nearer; a
//     pixel's first fragment puts it on the pixel list;
//  3. per listed pixel, the build walks the list, allocates a contiguous range, writes the fragments sorted nearest
//     first (16 B, INTERFACES 7.1) and sets heads = first | count << 24.
// The pixel buffer (header + list) stays for M's coverage composite and for the next frame's clear.
//   P[0] cull state UAV (raw; VS_COV_* words), cull args UAV (raw), raw fragments UAV, sorted fragments UAV
//   P[1] heads UAV (RWTexture2D<uint>), pixel buffer UAV (raw, COV_PIXEL_*), fragment capacity, pixel capacity
//   P[2] HiZ SRV (Texture2D<float> mip 0, UNX_NONE = none), view width, view height, HiZ mip 0 width | height << 16
//   P[3] visible SRV (uint2), lists SRV (raw), list capacity, views SRV
//   P[4] front-face sign (+1: front = negative signed area in y-down pixels, i.e. counter-clockwise on screen;
//        -1 mirrored), unused x 3
#ifndef UNX_COVERAGE_LAYER_HLSLI
#define UNX_COVERAGE_LAYER_HLSLI
#include "Passes/Visibility/VisibilityCommon.hlsli"

struct CoverageRawFragment  // 20 B, linked per pixel during the raster
{
    uint visId;
    float depth;  // device depth (reversed Z) at the covered region's centroid
    uint mask;    // 32-subsample coverage (Coverage.hlsli coverageSample)
    float area;   // exact area of the triangle inside the pixel, in pixels (x the alpha-tested fraction)
    uint next;    // older fragment of the same pixel + 1 (0 = end)
};

struct CoverageFragment  // 16 B, INTERFACES 7.1
{
    uint visId;
    float depth;
    uint coverageMask32;
    float area;
};

#define COV_STATE P[0].x
#define COV_ARGS P[0].y
#define COV_RAW P[0].z
#define COV_SORTED P[0].w
#define COV_HEADS P[1].x
#define COV_PIXELS P[1].y
#define COV_CAP_FRAGMENTS P[1].z
#define COV_CAP_PIXELS P[1].w
#define COV_HIZ P[2].x
#define COV_WIDTH P[2].y
#define COV_HEIGHT P[2].z
#define COV_HIZ_SIZE uint2(P[2].w & 0xFFFFu, P[2].w >> 16)
#define COV_VISIBLE P[3].x
#define COV_LISTS P[3].y
#define COV_LIST_CAPACITY P[3].z
#define COV_VIEWS P[3].w
#define COV_FRONT_SIGN asfloat(P[4].x)

// Pixel buffer (raw, persistent; ViewResources::coveragePixels): a header, then the pixel list (x | y << 16).
#define COV_PIXEL_ARGS 0u       // DispatchIndirect args over this frame's coverage pixels (64 per group)
#define COV_PIXEL_COUNT 3u      // this frame's coverage pixels (next frame: the pixels whose heads are cleared)
#define COV_PIXEL_FRAGMENTS 4u  // this frame's sorted fragments
#define COV_PIXEL_LIST 8u       // first list word

// Primitive flags (CoverageRaster.ms -> ps).
#define COV_FLAG_ALPHA 1u  // alpha-tested material
#define COV_FLAG_QUAD 2u   // near-clipped: the polygon is the quad (a, b, c, d)

uint coveragePack(uint2 p) { return p.x | (p.y << 16); }
uint2 coverageUnpack(uint v) { return uint2(v & 0xFFFFu, v >> 16); }

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
