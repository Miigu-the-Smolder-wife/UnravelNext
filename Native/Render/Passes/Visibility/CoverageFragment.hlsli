// V internal: one coverage fragment - a primitive's polygon inside one pixel - as the coverage pixel kernel
// (CoverageRaster.ps) and the compute rasteriser (CoverageRasterSw.hlsl) evaluate it, so both store the same record for
// the same polygon and pixel. Owner: V.
//   coverageFragmentGeometry: the layer's pixel tests, the exact area and centroid, the depth, band A occlusion and
//                             the cut behind the cover of the depth buckets drawn before (CoverageBuckets.hlsli);
//   coverageFragmentMask:     the 32-subsample mask, the alpha test, and the drop of a fragment without weight;
//   coverageFragmentRecord:   the record's words.
// A fragment is dropped (never stored) when the composite would give it weight zero whatever else the pixel holds:
//   - behind band A over its 3 x 3 neighbourhood (a band A edge pixel keeps its fragments for the E composite);
//   - behind the cover: the opaque fragments stored by earlier buckets fill the pixel's subsamples and all of them
//     are nearer (CoverageBuckets.hlsli);
//   - no weight of its own (visibility.coverage_drop_weightless): its area rounds to step 0 of the record's 10 bits and
//     it covers no subsample (weight = area x share, and it adds nothing to the mask union). Hair ribbons keep such
//     fragments: their owner picks the pixel's nearest and farthest hair record.
// visibility.coverage_statistics (COV_STATS): the fragments by outcome, one atomic per wave and counter.
#ifndef UNX_COVERAGE_FRAGMENT_HLSLI
#define UNX_COVERAGE_FRAGMENT_HLSLI
#include "Passes/Visibility/CoverageBuckets.hlsli"
#include "Passes/Visibility/CoveragePolygon.hlsli"

#ifndef COV_STATS
#define COV_STATS 0
#endif

#define COV_OUT_STORED 0u
#define COV_OUT_EMPTY 1u    // nothing of the primitive in the pixel, or not a pixel of the layer
#define COV_OUT_BAND_A 2u
#define COV_OUT_COVER 3u
#define COV_OUT_ALPHA 4u
#define COV_OUT_WEIGHT 5u

struct CoverageFragmentState
{
    bool live;
    uint outcome;       // COV_OUT_*
    CoverageSample cs;  // area, depth, mask
    float2 mid;         // the covered region's centroid
    uint tile, pixelInTile;
};

bool coverageAboveBandA(uint2 pixel, float depth)
{
    if (COV_HIZ == UNX_NONE) return true;
    Texture2D<float> hiz = ResourceDescriptorHeap[COV_HIZ];
    const uint2 size = COV_HIZ_SIZE;
    const uint2 t0 = min(uint2(max(int2(pixel) - 1, 0)) >> 1, size - 1), t1 = min((pixel + 1) >> 1, size - 1);
    float farthest = 1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 t = uint2((k & 1) ? t1.x : t0.x, (k & 2) ? t1.y : t0.y);
        farthest = min(farthest, hiz.Load(int3(t, 0)));
    }
    return depth >= farthest;  // reversed Z: nearer = larger
}

// Behind the cover of the buckets drawn before: the union of their opaque fragments at this pixel is full and every
// one of them is nearer (one 8 B load).
bool coverageBehindCover(uint key, float depth)
{
    if (COV_COVER == UNX_NONE) return false;
    ByteAddressBuffer cover = ResourceDescriptorHeap[COV_COVER];
    const uint2 c = cover.Load2(8 * key);
    return c.x == COV_MASK_FULL && asuint(depth) < ~c.y;
}

// Unit normal at screen point s: perspective-correct over the polygon's triangle holding s ((a, b, c), else (a, c, d)).
float3 coveragePolygonNormal(CoveragePolygon p, uint4 normals, float2 s, bool back)
{
    float3 w = coveragePolygonBarycentric(p.a.xy, p.b.xy, p.c.xy, s);
    float2 v1 = float2(p.b.w, p.c.w);
    uint2 n12 = normals.yz;
    if (p.quad && min(w.x, min(w.y, w.z)) < 0)
    {
        w = coveragePolygonBarycentric(p.a.xy, p.c.xy, p.d.xy, s);
        v1 = float2(p.c.w, p.d.w);
        n12 = normals.zw;
    }
    const float3 q = w * float3(p.a.w, v1);  // weights x 1 / w
    const float3 na = coverageOct32Decode(normals.x);
    float3 n = na * q.x + coverageOct32Decode(n12.x) * q.y + coverageOct32Decode(n12.y) * q.z;
    n = dot(n, n) > 1e-20 ? normalize(n) : na;
    return back ? -n : n;
}

// Hair ribbons (COV_FLAG_HAIR): u at screen point s, perspective-correct like the normals (asuint(u) per polygon vertex).
float coveragePolygonU(CoveragePolygon p, uint4 u, float2 s)
{
    float3 w = coveragePolygonBarycentric(p.a.xy, p.b.xy, p.c.xy, s);
    float2 v1 = float2(p.b.w, p.c.w);
    float2 u12 = asfloat(u.yz);
    if (p.quad && min(w.x, min(w.y, w.z)) < 0)
    {
        w = coveragePolygonBarycentric(p.a.xy, p.c.xy, p.d.xy, s);
        v1 = float2(p.c.w, p.d.w);
        u12 = asfloat(u.zw);
    }
    const float3 q = w * float3(p.a.w, v1);
    const float sum = q.x + q.y + q.z;
    return sum > 0 ? (asfloat(u.x) * q.x + u12.x * q.y + u12.y * q.z) / sum : asfloat(u.x);
}

// The record's last word: the interpolated normal, or u for a hair ribbon.
uint coveragePackPrimitive(CoveragePolygon poly, uint flags, uint4 normals, float2 mid, float area, uint pixelInTile)
{
    if ((flags & COV_FLAG_HAIR) != 0) return coveragePackHair(coveragePolygonU(poly, normals, mid), area, pixelInTile);
    return coveragePackFragment(coveragePolygonNormal(poly, normals, mid, (flags & COV_FLAG_BACK) != 0), area, pixelInTile);
}

// Steps 1 to 3 of the pixel kernel: the pixel is one of the layer's for this primitive, the exact area and centroid,
// the depth there, band A occlusion, the cover. The depth is kept inside the polygon's vertex depths: it is the plane's
// value at a point of the polygon, so only rounding in the barycentric weights of a sliver can leave that range, and the
// mesh kernel's triangle tests (CoverageBuckets.hlsli) rely on it.
CoverageFragmentState coverageFragmentGeometry(CoveragePolygon poly, uint flags, uint2 pixel)
{
    CoverageFragmentState f;
    f.cs = (CoverageSample)0;
    f.mid = 0;
    f.outcome = COV_OUT_EMPTY;
    f.tile = (pixel.y / COV_TILE_PX) * COV_TILES_X + pixel.x / COV_TILE_PX;
    f.pixelInTile = (pixel.x % COV_TILE_PX) + COV_TILE_PX * (pixel.y % COV_TILE_PX);
    f.live = pixel.x < COV_WIDTH && pixel.y < COV_HEIGHT;
    if (f.live && (flags & COV_FLAG_WATER_EDGE) != 0) f.live = coverageWaterEdge(pixel);  // v1.64: water layer edges only
    if (f.live && (flags & COV_FLAG_TRANSLUCENT) != 0) f.live = coverageTranslucentRecord(pixel);  // A6: class 2 pixels only
    if (f.live)
    {
        f.cs = coveragePolygonGeometry(poly, float2(pixel), f.mid);
        f.live = f.cs.area > 0;
    }
    if (f.live)
    {
        f.cs.depth = clamp(f.cs.depth, min(min(poly.a.z, poly.b.z), min(poly.c.z, poly.d.z)), max(max(poly.a.z, poly.b.z), max(poly.c.z, poly.d.z)));
        f.outcome = COV_OUT_STORED;
        if (!coverageAboveBandA(pixel, f.cs.depth))
        {
            f.live = false;
            f.outcome = COV_OUT_BAND_A;
        }
        else if (coverageBehindCover(f.tile * COV_TILE_PIXELS + f.pixelInTile, f.cs.depth))
        {
            f.live = false;
            f.outcome = COV_OUT_COVER;
        }
    }
    return f;
}

// Steps 4: the subsample mask, the alpha test, and the fragment without weight.
void coverageFragmentMask(CoveragePolygon poly, uint flags, uint material, uint2 pixel, inout CoverageFragmentState f)
{
    if (!f.live) return;
    f.cs.mask = coveragePolygonMask(poly, float2(pixel));
    if ((flags & COV_FLAG_ALPHA) != 0)
    {
        coveragePolygonAlpha(poly, material, float2(pixel), f.mid, f.cs);
        if (!(f.cs.area > 0))
        {
            f.live = false;
            f.outcome = COV_OUT_ALPHA;
            return;
        }
    }
    // (the record's area step, CoverageTiles.hlsli coveragePackFragment / coveragePackHair)
    if (COV_RASTER_DROP_WEIGHTLESS && f.cs.mask == 0 && (uint)round(saturate(f.cs.area) * 1023) == 0 && (flags & COV_FLAG_HAIR) == 0)
    {
        f.live = false;
        f.outcome = COV_OUT_WEIGHT;
    }
}

// Step 6's values: the record (16 B) of a live fragment.
uint4 coverageFragmentRecord(CoveragePolygon poly, uint visId, uint flags, uint4 normals, CoverageFragmentState f)
{
    const uint depthBits = asuint(f.cs.depth) | ((flags & COV_FLAG_OPAQUE) != 0 ? 0u : COV_DEPTH_SEE_THROUGH);
    return uint4(visId, depthBits, f.cs.mask, coveragePackPrimitive(poly, flags, normals, f.mid, f.cs.area, f.pixelInTile));
}

// The fragments of a wave by outcome (COV_STATS; uniform control flow: every lane of the wave calls it once per
// fragment slot, 'counted' false for a lane without one).
void coverageFragmentStatistics(RWByteAddressBuffer state, bool counted, uint outcome)
{
#if COV_STATS
    const uint evaluated = WaveActiveCountBits(counted && outcome != COV_OUT_EMPTY);
    const uint bandA = WaveActiveCountBits(counted && outcome == COV_OUT_BAND_A), cover = WaveActiveCountBits(counted && outcome == COV_OUT_COVER);
    const uint alpha = WaveActiveCountBits(counted && outcome == COV_OUT_ALPHA), weight = WaveActiveCountBits(counted && outcome == COV_OUT_WEIGHT);
    if (WaveIsFirstLane())
    {
        if (evaluated > 0) state.InterlockedAdd(4 * VS_COV_EVALUATED, evaluated);
        if (bandA > 0) state.InterlockedAdd(4 * VS_COV_CUT_BAND_A, bandA);
        if (cover > 0) state.InterlockedAdd(4 * VS_COV_CUT_COVER, cover);
        if (alpha > 0) state.InterlockedAdd(4 * VS_COV_CUT_ALPHA, alpha);
        if (weight > 0) state.InterlockedAdd(4 * VS_COV_CUT_WEIGHT, weight);
    }
#endif
}

#endif
