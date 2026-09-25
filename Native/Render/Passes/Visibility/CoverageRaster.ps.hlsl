// unx-kernel: ps_6_6 main
// unx-variants: STAGE=0,1,2
// Coverage layer pixel kernel (band B, CoverageLayer.hlsli, CoverageTiles.hlsli), conservative rasterisation, no render
// target or depth (STAGE 0; STAGE 1 and 2 are measurement variants, visibility.coverage_debug_stage: 1 stops after the
// fragment's math, 2 after the tile counters, so the gate can split the kernel's cost):
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
//  6. append to the pixel's 8 x 8 tile: the wave's fragments of one tile take one count atomic (and one depth-range
//     update); the tile's i-th fragment goes to record i % 64 of its chunk i / 64, which the first wave needing it takes
//     from the pool and publishes in the tile's chunk table, or past the table's slots in the tile's extension tables,
//     by compare-and-swap (a wave losing the race uses the winner's chunk; no waiting). A tile's first fragment puts it
//     on the tile list. A record of a see-through material (glass, water) has COV_DEPTH_SEE_THROUGH in its depth word.
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

// A chunk from the pool (index + 1; 0: the pool ran out). A chunk that becomes an extension table is zeroed, and the
// fence orders the zeroes before its publication.
uint coverageTakeChunk(bool table, RWByteAddressBuffer state, RWStructuredBuffer<uint4> records)
{
    uint fresh = 0;
    state.InterlockedAdd(4 * VS_COV_CHUNKS, 1, fresh);
    if (fresh >= COV_CAP_CHUNKS)
    {
        state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_COVERAGE);
        return 0;
    }
    if (table)
    {
        for (uint e = 0; e < COV_CHUNK_RECORDS; ++e) records[fresh * COV_CHUNK_RECORDS + e] = 0;
        DeviceMemoryBarrier();
    }
    return fresh + 1;
}

// The chunk (index + 1) published at byte 'address' of the chunk table or the tile headers; if none is there yet, one is
// taken from the pool and published by compare-and-swap (a wave losing the race uses the winner's chunk). Reads are
// atomic, so they see other waves' publications whatever the caches. 0: the pool ran out.
uint coverageChunkAt(RWByteAddressBuffer buf, uint address, bool table, RWByteAddressBuffer state, RWStructuredBuffer<uint4> records)
{
    uint found = 0;
    buf.InterlockedOr(address, 0, found);
    if (found != 0) return found;
    const uint fresh = coverageTakeChunk(table, state, records);
    if (fresh == 0) return 0;
    uint previous = 0;
    buf.InterlockedCompareExchange(address, 0, fresh, previous);
    if (previous == 0) return fresh;
    state.InterlockedAdd(4 * VS_COV_LOST, 1);
    return previous;
}

// The same for word w of the extension table in chunk t (index + 1): element (t - 1) * 64 + w / 4, component w % 4.
uint coverageExtChunkAt(RWStructuredBuffer<uint4> records, uint t, uint w, bool table, RWByteAddressBuffer state)
{
    const uint e = (t - 1) * COV_CHUNK_RECORDS + w / 4, c = w % 4;
    uint found = 0;
    InterlockedOr(records[e][c], 0, found);
    if (found != 0) return found;
    const uint fresh = coverageTakeChunk(table, state, records);
    if (fresh == 0) return 0;
    uint previous = 0;
    InterlockedCompareExchange(records[e][c], 0, fresh, previous);
    if (previous == 0) return fresh;
    state.InterlockedAdd(4 * VS_COV_LOST, 1);
    return previous;
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

void main(float4 position : SV_Position, nointerpolation uint visId : VISID, nointerpolation uint flags : COVFLAGS, nointerpolation uint material : MATERIAL,
          nointerpolation float4 a : TRIA, nointerpolation float4 b : TRIB, nointerpolation float4 c : TRIC, nointerpolation float4 d : TRID,
          nointerpolation float4 tab : UVAB, nointerpolation float4 tcd : UVCD, nointerpolation uint4 normals : NRMS)
{
    if (IsHelperLane()) return;  // no derivatives are taken (explicit gradients)
    const uint2 pixel = uint2(position.xy);
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
    if (live)
    {
        cs = coveragePolygonGeometry(poly, float2(pixel), mid);
        live = cs.area > 0 && coverageAboveBandA(pixel, cs.depth);
    }
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
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
#if STAGE != 0
    {
        // Measurement: the record's values are computed as in STAGE 0 and folded into one wave value, so the compiler
        // keeps the work; the fragments are counted.
        const float3 normal = live ? coveragePolygonNormal(poly, normals, mid, (flags & COV_FLAG_BACK) != 0) : float3(0, 0, 0);
        const uint folded = WaveActiveBitXor(live ? cs.mask ^ asuint(cs.depth) ^ coveragePackFragment(normal, cs.area, pixelInTile) : 0u);
        if (WaveIsFirstLane() && folded == 0x9E3779B9u) state.InterlockedAdd(4 * VS_COV_LOST, 1);
    }
#endif
#if STAGE == 1
    {
        const uint counted = WaveActiveCountBits(live);
        if (counted > 0 && WaveIsFirstLane()) state.InterlockedAdd(4 * VS_COV_FRAGMENTS, counted);
        return;
    }
#endif

    // Slots in the tiles: one count atomic and one depth-range update per tile present in the wave.
    uint index = 0;
    bool first = false, pending = live;
    uint tileRounds = 0;
    while (WaveActiveAnyTrue(pending))
    {
        if (++tileRounds > WaveGetLaneCount())
        {
            if (WaveIsFirstLane()) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);
            break;
        }
        // The first pending lane's tile, read by the pending lanes only (a non-pending first lane would match nobody).
        uint lead = 0;
        if (pending) lead = WaveReadLaneFirst(tile);
        if (pending && tile == lead)
        {
            const uint n = WaveActiveCountBits(true), rank = WavePrefixCountBits(true);
            const float zNear = WaveActiveMax(cs.depth), zFar = WaveActiveMin(cs.depth);
            uint base = 0;
            if (WaveIsFirstLane())
            {
                const uint h = 4 * tile * COV_TILE_WORDS;
                headers.InterlockedAdd(h + 4 * COV_TILE_COUNT, n, base);
                headers.InterlockedMax(h + 4 * COV_TILE_ZNEAR, asuint(zNear));
                headers.InterlockedMin(h + 4 * COV_TILE_ZFAR, asuint(zFar));
            }
            index = WaveReadLaneFirst(base) + rank;
            first = index == 0;
            pending = false;
        }
    }
    const uint fragments = WaveActiveCountBits(live);
    if (fragments > 0 && WaveIsFirstLane()) state.InterlockedAdd(4 * VS_COV_FRAGMENTS, fragments);
    const uint listSlot = waveAppend(state, VS_COV_TILES, first ? 1 : 0, COV_TILES, OVERFLOW_COVERAGE);
    if (first && listSlot < COV_TILES)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
        list.Store(4 * (COV_LIST_TILES + listSlot), tile);
    }
#if STAGE == 2
    return;
#endif

    // Chunks: one lookup (and, for a new chunk, one pool allocation and compare-and-swap) per (tile, chunk) present in
    // the wave. Past the table's slots the tile's extension tree holds the chunks (at most 3 node reads): no fragment
    // is dropped while the pool lasts.
    const uint ordinal = index / COV_CHUNK_RECORDS;
    RWByteAddressBuffer table = ResourceDescriptorHeap[COV_CHUNK_TABLE];
    RWStructuredBuffer<uint4> records = ResourceDescriptorHeap[COV_RECORDS];
    uint chunk = 0;  // chunk index + 1 (0: the pool ran out)
    pending = live;
    uint rounds = 0;  // at most one round per lane (a lane finishes in the round its (tile, chunk) leads)
    while (WaveActiveAnyTrue(pending))
    {
        if (++rounds > WaveGetLaneCount())
        {
            if (WaveIsFirstLane()) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);
            break;
        }
        uint2 lead = 0;
        if (pending) lead = WaveReadLaneFirst(uint2(tile, ordinal));
        if (pending && all(uint2(tile, ordinal) == lead))
        {
            uint found = 0;
            if (WaveIsFirstLane())
            {
                const uint slots = COV_TABLE_SLOTS;
                if (ordinal < slots) found = coverageChunkAt(table, 4 * (tile * slots + ordinal), false, state, records);
                else
                {
                    uint e = ordinal - slots, rootWord, digits;
                    if (!coverageExtPath(e, rootWord, digits)) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_COVERAGE_DEPTH);
                    else
                    {
                        // Root (a node), its word (the chunk when direct, else a subtree node), then one node per digit;
                        // the last word read is the chunk itself.
                        uint t = coverageChunkAt(headers, 4 * (tile * COV_TILE_WORDS + COV_TILE_EXT), true, state, records);
                        if (t != 0) t = coverageExtChunkAt(records, t, rootWord, digits > 0, state);
                        [unroll] for (uint k = 3; k > 0; --k)
                            if (k <= digits && t != 0) t = coverageExtChunkAt(records, t, (e >> (8 * (k - 1))) & 0xFFu, k > 1, state);
                        found = t;
                    }
                }
            }
            chunk = WaveReadLaneFirst(found);
            pending = false;
        }
    }
    if (chunk != 0)
    {
        const float3 normal = coveragePolygonNormal(poly, normals, mid, (flags & COV_FLAG_BACK) != 0);
        const uint depthBits = asuint(cs.depth) | ((flags & COV_FLAG_OPAQUE) != 0 ? 0u : COV_DEPTH_SEE_THROUGH);
        records[(chunk - 1) * COV_CHUNK_RECORDS + index % COV_CHUNK_RECORDS] = uint4(visId, depthBits, cs.mask, coveragePackFragment(normal, cs.area, pixelInTile));
    }
}
