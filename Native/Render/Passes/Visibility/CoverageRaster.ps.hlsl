// unx-kernel: ps_6_6 main
// unx-variants: STAGE=0,1,2,3,4
// Coverage layer pixel kernel (band B, CoverageLayer.hlsli, CoverageTiles.hlsli), conservative rasterisation, no render
// target or depth (STAGE 0; the others are measurement variants, visibility.coverage_debug_stage, that split the
// kernel's cost: 3 returns at once (mesh kernel + rasteriser), 4 after area, depth and the band A test, 1 after the
// fragment's math, 2 after the append atomic; the measurement variants count the kernel's invocations and store nothing):
//  1. the exact area of the primitive's polygon (triangle, or the near-clipped quad as two triangles) inside this pixel
//     and the covered region's centroid (Coverage.hlsli); zero area (conservative raster's touching pixels) writes
//     nothing;
//  2. depth at the centroid (z/w is affine in screen space over the plane of the triangle);
//  3. band A occlusion: dropped when every band A surface over the pixel's 3 x 3 neighbourhood is nearer (HiZ mip 0:
//     farthest depth of 2 x 2 blocks). A band A edge pixel keeps its band B fragments (the E composite needs them). No
//     cut behind nearer band B fragments here: per-fragment atomics for that cost more than they save (design 4.6,
//     DesignBench); M's composite applies the mask-union rule in its own sort;
//  4. the 32-subsample mask; alpha-tested materials clear the subsamples whose texture alpha fails and scale the area by
//     the passing fraction. The uv is perspective-correct (uv/w and 1/w are affine in screen space) and the texture
//     footprint is one subsample's cell (the pixel's gradients x 1/sqrt(32)): 32 samples of the alpha estimate its
//     coverage inside the pixel without the pixel-wide blur a pixel footprint would threshold. A polygon covering no
//     subsample takes the test at its centroid with the same footprint for its whole area;
//  5. the normal at the centroid: perspective-correct interpolation of the vertex normals over the triangle of the
//     polygon holding the centroid, turned towards the viewer for a primitive seen from behind;
//  6. append to the stream: one atomic per wave, the record (16 B) and its key tile x 64 + pixel (4 B); CoverageBuild
//     sorts the stream into the tiles' pixel-major ranges. A record of a see-through material (glass, water) has
//     COV_DEPTH_SEE_THROUGH in its depth word. Past the capacity the fragment is lost and OVERFLOW_COVERAGE is set (the
//     pool grows from the next completed frame).
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "Passes/Visibility/CoveragePolygon.hlsli"

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

void main(float4 position : SV_Position, nointerpolation uint visId : VISID, nointerpolation uint flags : COVFLAGS, nointerpolation uint material : MATERIAL,
          nointerpolation float4 a : TRIA, nointerpolation float4 b : TRIB, nointerpolation float4 c : TRIC, nointerpolation float4 d : TRID,
          nointerpolation float4 tab : UVAB, nointerpolation float4 tcd : UVCD, nointerpolation uint4 normals : NRMS)
{
    if (IsHelperLane()) return;  // no derivatives are taken (explicit gradients)
    const uint2 pixel = uint2(position.xy);
#if STAGE != 0
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_STATE];
        const uint invocations = WaveActiveCountBits(true);
        if (WaveIsFirstLane()) counters.InterlockedAdd(4 * VS_COV_INVOCATIONS, invocations);
    }
#endif
#if STAGE == 3
    return;
#endif
    CoveragePolygon poly;
    poly.a = a;
    poly.b = b;
    poly.c = c;
    poly.d = d;
    poly.ta = tab.xy;
    poly.tb = tab.zw;
    poly.tc = tcd.xy;
    poly.td = tcd.zw;
    poly.quad = (flags & COV_FLAG_QUAD) != 0;
    float2 mid = 0;
    CoverageSample cs = (CoverageSample)0;
    const uint tile = (pixel.y / COV_TILE_PX) * COV_TILES_X + pixel.x / COV_TILE_PX;
    const uint pixelInTile = (pixel.x % COV_TILE_PX) + COV_TILE_PX * (pixel.y % COV_TILE_PX);
    bool live = pixel.x < COV_WIDTH && pixel.y < COV_HEIGHT;
    if (live && (flags & COV_FLAG_WATER_EDGE) != 0) live = coverageWaterEdge(pixel);  // v1.64: water layer edges only
    if (live && (flags & COV_FLAG_TRANSLUCENT) != 0) live = coverageTranslucentRecord(pixel);  // A6: class 2 pixels only
    if (live)
    {
        cs = coveragePolygonGeometry(poly, float2(pixel), mid);
        live = cs.area > 0 && coverageAboveBandA(pixel, cs.depth);
    }
#if STAGE == 4
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_STATE];
        const uint folded = WaveActiveBitXor(live ? asuint(cs.depth) ^ asuint(cs.area) ^ asuint(mid.x + mid.y) : 0u);
        const uint counted = WaveActiveCountBits(live);
        if (WaveIsFirstLane())
        {
            if (counted > 0) counters.InterlockedAdd(4 * VS_COV_MEASURED, counted);
            if (folded == 0x9E3779B9u) counters.InterlockedAdd(4 * VS_COV_MEASURED, 0x80000000u);
        }
        return;
    }
#endif
    if (live)
    {
        cs.mask = coveragePolygonMask(poly, float2(pixel));
        if ((flags & COV_FLAG_ALPHA) != 0)
        {
            coveragePolygonAlpha(poly, material, float2(pixel), mid, cs);
            live = cs.area > 0;
        }
    }
    // From here every lane takes part in the wave operations; 'live' selects the lanes with a fragment.
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
#if STAGE != 0
    {
        // Measurement: the record's values are computed as in STAGE 0 and folded into one wave value, so the compiler
        // keeps the work; the fragments are counted.
        const uint folded = WaveActiveBitXor(live ? cs.mask ^ asuint(cs.depth) ^ coveragePackPrimitive(poly, flags, normals, mid, cs.area, pixelInTile) : 0u);
        if (WaveIsFirstLane() && folded == 0x9E3779B9u) state.InterlockedAdd(4 * VS_COV_MEASURED, 0x80000000u);
    }
#endif
#if STAGE == 1
    {
        const uint counted = WaveActiveCountBits(live);
        if (counted > 0 && WaveIsFirstLane()) state.InterlockedAdd(4 * VS_COV_MEASURED, counted);
        return;
    }
#elif STAGE == 2
    // The append atomic of STAGE 0 on the measurement word (the stream is not written, so nothing downstream reads it).
    waveAppend(state, VS_COV_MEASURED, live ? 1 : 0, 0xFFFFFFFFu, 0);
    return;
#else
    const uint slot = waveAppend(state, VS_COV_FRAGMENTS, live ? 1 : 0, COV_CAP, OVERFLOW_COVERAGE);
    if (live && slot < COV_CAP)
    {
        RWStructuredBuffer<uint4> stream = ResourceDescriptorHeap[COV_STREAM];
        RWByteAddressBuffer keys = ResourceDescriptorHeap[COV_KEYS];
        const uint depthBits = asuint(cs.depth) | ((flags & COV_FLAG_OPAQUE) != 0 ? 0u : COV_DEPTH_SEE_THROUGH);
        stream[slot] = uint4(visId, depthBits, cs.mask, coveragePackPrimitive(poly, flags, normals, mid, cs.area, pixelInTile));
        keys.Store(4 * slot, tile * COV_TILE_PIXELS + pixelInTile);
    }
#endif
}
