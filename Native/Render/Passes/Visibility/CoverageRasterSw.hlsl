// unx-kernel: cs_6_6 main
// unx-variants: STATS=0,1
// Coverage layer compute rasteriser (visibility.coverage_compute_raster; the reference's software raster applied to the
// coverage layer): one group of 64 threads per sorted entry of a depth bucket (CoverageBuckets.hlsli), before the
// bucket's mesh raster. The cluster's vertices and primitives are the mesh kernel's (CoveragePrimitive.hlsli:
// deformVertex, the near clip, the same culls). A triangle the near plane does not cut and whose rectangle holds at
// most COV_SW_PIXELS pixels of the view is rasterised here: its thread visits those pixels and evaluates each as the
// pixel kernel does (CoverageFragment.hlsli: exact area and centroid, depth, band A, cover, mask, alpha, normal) and
// appends the records to the same stream (one atomic per wave and pixel step). The hardware's conservative raster
// invokes the pixel kernel on every pixel the triangle touches and the kernel stores those with area; here every pixel
// of the rectangle is evaluated and those with area stored: the same pixels, the same functions of the same polygon
// (equal up to the two compilations' float rounding). No triangle set-up, no primitive attributes.
// The triangles taken are published as a bit per triangle beside the sorted entry (4 words); the mesh kernel skips
// exactly those, so every triangle is drawn by one of the two whatever the last bit of either's projection is.
// Work per group: the cluster's vertices (<= 128), two rounds of 64 triangles, COV_SW_PIXELS pixel steps per round.
// Root constants: CoverageLayer.hlsli's raster layout and CoverageBuckets.hlsli's (the bins as a UAV).
#define COV_STATS STATS
#include "Passes/Visibility/CoverageFragment.hlsli"
#include "Passes/Visibility/CoveragePrimitive.hlsli"

groupshared float4 gs_clip[128];
groupshared float2 gs_uv[128];
groupshared float3 gs_normal[128];
groupshared float3 gs_world[128];
groupshared uint gs_taken[4];

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    const uint capacity = COV_LIST_CAPACITY, bucket = COV_RASTER_BUCKET;
    const uint index = group.x + group.y * 65535;
    const uint sorted = bins.Load(4 * (COVB_FIRST + bucket)) + index;
    if (index >= bins.Load(4 * (COVB_COUNT + bucket)) || sorted >= capacity) return;  // uniform over the group
    const CoverageDraw draw = coverageDraw(bins.Load(4 * covbSorted(capacity, sorted)), true, false);
    if (lane < 4) gs_taken[lane] = 0;
    // A cluster behind the tiles' cover: nothing taken (the mesh kernel makes the same test and outputs nothing).
    const bool hidden = coverageDrawBehindCover(draw);
    const uint vertexCount = hidden ? 0 : min(clusterVertexCount(draw.cl), 128u), triangleCount = hidden ? 0 : min(clusterTriangleCount(draw.cl), 128u);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[draw.cl.vertexOffset + i];
        const DeformedVertex dv = deformVertex(draw.inst, draw.mesh, meshVertex);
        gs_clip[i] = viewModelClip(draw.inst, mul(draw.v.viewProj, float4(dv.world, 1)));
        gs_uv[i] = loadVertex(draw.mesh, meshVertex).uv;
        gs_normal[i] = dv.normal;
        gs_world[i] = dv.world;
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    RWStructuredBuffer<uint4> stream = ResourceDescriptorHeap[COV_STREAM];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[COV_KEYS];
    uint triangles = 0, fragments = 0;
    for (uint base = 0; base < triangleCount; base += 64)  // uniform over the group: at most two rounds
    {
        const uint t = base + lane;
        CoveragePrimitive cp = (CoveragePrimitive)0;
        int2 first = 0;
        uint2 size = 0;
        bool small = false;
        if (t < triangleCount)
        {
            const uint packed = clusterTriangles[draw.cl.triangleOffset + t];
            const uint3 tri = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
            const float4 p[3] = { gs_clip[tri.x], gs_clip[tri.y], gs_clip[tri.z] };
            const float2 uv[3] = { gs_uv[tri.x], gs_uv[tri.y], gs_uv[tri.z] };
            const float3 nv[3] = { gs_normal[tri.x], gs_normal[tri.y], gs_normal[tri.z] };
            const float3 world[3] = { gs_world[tri.x], gs_world[tri.y], gs_world[tri.z] };
            cp = coveragePrimitive(draw, t, tri, p, uv, nv, world);
            small = coveragePrimitiveSmall(cp, first, size);
        }
        if (small)
        {
            InterlockedOr(gs_taken[(t >> 5) & 3], 1u << (t & 31));
            ++triangles;
        }
        CoveragePolygon poly;
        poly.a = cp.a;
        poly.b = cp.b;
        poly.c = cp.c;
        poly.d = cp.d;
        poly.ta = cp.tab.xy;
        poly.tb = cp.tab.zw;
        poly.tc = cp.tcd.xy;
        poly.td = cp.tcd.zw;
        poly.quad = false;  // (a near-clipped triangle is never taken)
        const uint width = max(size.x, 1u), pixels = small ? size.x * size.y : 0u;
        for (uint k = 0; k < COV_SW_PIXELS; ++k)  // the rectangle's pixels, row by row; every lane of the wave takes each step
        {
            const bool here = k < pixels;
            CoverageFragmentState f = (CoverageFragmentState)0;
            f.outcome = COV_OUT_EMPTY;
            if (here)
            {
                const uint2 pixel = uint2(first) + uint2(k % width, k / width);
                f = coverageFragmentGeometry(poly, cp.flags, pixel);
                coverageFragmentMask(poly, cp.flags, cp.material, pixel, f);
            }
            const bool live = here && f.live;
            coverageFragmentStatistics(state, here, f.outcome);
            const uint slot = waveAppend(state, VS_COV_FRAGMENTS, live ? 1 : 0, COV_CAP, OVERFLOW_COVERAGE);
            if (live && slot < COV_CAP)
            {
                stream[slot] = coverageFragmentRecord(poly, cp.visId, cp.flags, cp.normals, f);
                keys.Store(4 * slot, f.tile * COV_TILE_PIXELS + f.pixelInTile);
                ++fragments;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 4) bins.Store(4 * (covbTaken(capacity, sorted) + lane), gs_taken[lane]);
    const uint triangleSum = WaveActiveSum(triangles), fragmentSum = WaveActiveSum(fragments);
    if (WaveIsFirstLane())
    {
        if (triangleSum > 0) state.InterlockedAdd(4 * VS_COV_TRIANGLES_SW, triangleSum);
        if (fragmentSum > 0) state.InterlockedAdd(4 * VS_COV_FRAGMENTS_SW, fragmentSum);
    }
}
