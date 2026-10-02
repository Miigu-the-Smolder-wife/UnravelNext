// unx-kernel: ps_6_6 main
// unx-variants: STAGE=0,1,2,3,4 STATS=0,1
// Coverage layer pixel kernel (band B, CoverageLayer.hlsli, CoverageTiles.hlsli), conservative rasterisation, no render
// target or depth (STAGE 0; the others are measurement variants, visibility.coverage_debug_stage, that split the
// kernel's cost: 3 returns at once (mesh kernel + rasteriser), 4 after area, depth and the band A test, 1 after the
// fragment's math, 2 after the append atomic; the measurement variants count the kernel's invocations and store nothing).
// The fragment itself is CoverageFragment.hlsli's (the compute rasteriser evaluates the same functions):
//  1. the exact area of the primitive's polygon (triangle, or the near-clipped quad as two triangles) inside this pixel
//     and the covered region's centroid (Coverage.hlsli); zero area (conservative raster's touching pixels) writes
//     nothing;
//  2. depth at the centroid (z/w is affine in screen space over the plane of the triangle);
//  3. band A occlusion: dropped when every band A surface over the pixel's 3 x 3 neighbourhood is nearer (HiZ mip 0:
//     farthest depth of 2 x 2 blocks). A band A edge pixel keeps its band B fragments (the E composite needs them).
//     Behind nearer band B fragments: no per-fragment atomics (they cost more than they save, design 4.6, DesignBench);
//     with depth buckets (CoverageBuckets.hlsli) one load of the cover the earlier buckets left drops a fragment behind
//     a full union of nearer opaque fragments; M's composite applies the mask-union rule to what is stored;
//  4. the 32-subsample mask; alpha-tested materials clear the subsamples whose texture alpha fails and scale the area by
//     the passing fraction. The uv is perspective-correct (uv/w and 1/w are affine in screen space) and the texture
//     footprint is one subsample's cell (the pixel's gradients x 1/sqrt(32)): 32 samples of the alpha estimate its
//     coverage inside the pixel without the pixel-wide blur a pixel footprint would threshold. A polygon covering no
//     subsample takes the test at its centroid with the same footprint for its whole area. A fragment without weight
//     (area under half a record step, no subsample) is dropped;
//  5. the normal at the centroid: perspective-correct interpolation of the vertex normals over the triangle of the
//     polygon holding the centroid, turned towards the viewer for a primitive seen from behind;
//  6. append to the stream: one atomic per wave, the record (16 B) and its key tile x 64 + pixel (4 B); CoverageBuild
//     sorts the stream into the tiles' pixel-major ranges. A record of a see-through material (glass, water) has
//     COV_DEPTH_SEE_THROUGH in its depth word. Past the capacity the fragment is lost and OVERFLOW_COVERAGE is set (the
//     pool grows from the next completed frame).
// STATS=1 (visibility.coverage_statistics): the fragments by outcome into the cull state (per-wave atomics: a
// measurement of counts, not of time).
#define COV_STATS STATS
#include "Passes/Visibility/CoverageFragment.hlsli"

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
    CoverageFragmentState f = coverageFragmentGeometry(poly, flags, pixel);
#if STAGE == 4
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_STATE];
        const uint folded = WaveActiveBitXor(f.live ? asuint(f.cs.depth) ^ asuint(f.cs.area) ^ asuint(f.mid.x + f.mid.y) : 0u);
        const uint counted = WaveActiveCountBits(f.live);
        if (WaveIsFirstLane())
        {
            if (counted > 0) counters.InterlockedAdd(4 * VS_COV_MEASURED, counted);
            if (folded == 0x9E3779B9u) counters.InterlockedAdd(4 * VS_COV_MEASURED, 0x80000000u);
        }
        return;
    }
#endif
    coverageFragmentMask(poly, flags, material, pixel, f);
    const bool live = f.live;
    // From here every lane takes part in the wave operations; 'live' selects the lanes with a fragment.
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    coverageFragmentStatistics(state, true, f.outcome);
#if STAGE != 0
    {
        // Measurement: the record's values are computed as in STAGE 0 and folded into one wave value, so the compiler
        // keeps the work; the fragments are counted.
        const uint folded = WaveActiveBitXor(live ? f.cs.mask ^ asuint(f.cs.depth) ^ coveragePackPrimitive(poly, flags, normals, f.mid, f.cs.area, f.pixelInTile) : 0u);
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
        stream[slot] = coverageFragmentRecord(poly, visId, flags, normals, f);
        keys.Store(4 * slot, f.tile * COV_TILE_PIXELS + f.pixelInTile);
    }
#endif
}
