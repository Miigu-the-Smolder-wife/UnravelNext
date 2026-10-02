// unx-kernel: ms_6_6 main
// Coverage layer raster (band B, CoverageLayer.hlsli): one mesh-shader group per band B list entry (both cull phases).
// Vertices go through deformVertex like band A. Each primitive carries what the pixel kernel needs for the exact
// evaluation inside every pixel it touches under conservative rasterisation: the vis id, the material and flags, and
// the triangle clipped to the near plane (z <= w, reversed Z) as a polygon of 3 or 4 vertices in viewport pixels with
// device depth z/w and 1/w, plus their uvs (perspective-correct alpha tests). The hardware clips the same triangle, so
// the pixels it shades lie in that polygon. Culled here: nothing left after the near clip, zero area, and back faces of
// one-sided materials in views that cull back faces (the front sign follows the view's mirroring). The polygon's vertex
// normals (world, octahedral 16 + 16 bits, so the record's 8 + 8 bits are the only coarse rounding) go along for the
// record's interpolated normal; COV_FLAG_BACK marks a
// primitive seen from behind (two-sided materials), whose normals the pixel kernel turns towards the viewer.
// (The primitive is CoveragePrimitive.hlsli's, shared with the compute rasteriser.)
// A6 (v1.67): the same kernel draws the translucent lists (COV_RASTER_LIST, COV_RASTER_TRANSLUCENT): a mixed sheet cluster
// there keeps its band A triangles (its band B ones are in LIST_B), and every primitive carries COV_FLAG_TRANSLUCENT (the
// pixel kernel keeps only the pixels of translucent class 2).
// Depth buckets (CoverageBuckets.hlsli; COV_BINS): the group's entry is sorted entry 'first of the bucket + group' of
// the binned list instead of list entry 'group'; a cluster behind the tiles' cover outputs nothing; triangles the
// compute rasteriser took (COV_RASTER_COMPUTE: bit per triangle beside the sorted entry) are skipped; with
// visibility.coverage_triangle_cull a triangle outside the view, behind band A or behind the tiles' cover is culled
// (CoveragePrimitive.hlsli). Counted per group into the cull state: triangles sent to the rasteriser and those culled
// by each of the two occlusion tests, clusters behind the cover.
#include "Passes/Visibility/CoveragePrimitive.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
};

struct PrimitiveOut
{
    uint visId : VISID;
    uint flags : COVFLAGS;
    uint material : MATERIAL;
    float4 a : TRIA;  // x, y (viewport pixels), z / w, 1 / w
    float4 b : TRIB;
    float4 c : TRIC;
    float4 d : TRID;  // = c unless COV_FLAG_QUAD
    float4 tab : UVAB;
    float4 tcd : UVCD;
    uint4 normals : NRMS;  // coverageOct32 of a, b, c, d
    bool cull : SV_CullPrimitive;
};

groupshared float4 gs_clip[128];
groupshared float2 gs_uv[128];
groupshared float3 gs_normal[128];
groupshared float3 gs_world[128];  // mixed sheet clusters: the per-triangle band test

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[128],
          out indices uint3 tris[128])
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];  // the pass writes it (pixel kernel counters)
    const uint capacity = COV_LIST_CAPACITY;
    const uint list = COV_RASTER_LIST;
    const bool translucent = COV_RASTER_TRANSLUCENT;
    const uint index = group.x + group.y * 65535;
    const bool binned = COV_BINS != UNX_NONE;
    bool valid = false;  // uniform over the group
    uint listEntry = 0;
    uint4 taken = 0;     // triangles of this entry the compute rasteriser took
    if (binned)
    {
        ByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
        const uint bucket = COV_RASTER_BUCKET;
        const uint sorted = bins.Load(4 * (COVB_FIRST + bucket)) + index;
        valid = index < bins.Load(4 * (COVB_COUNT + bucket)) && sorted < capacity;
        if (valid)
        {
            listEntry = bins.Load(4 * covbSorted(capacity, sorted));
            if (COV_RASTER_COMPUTE) taken = bins.Load4(4 * covbTaken(capacity, sorted));
        }
    }
    else
    {
        ByteAddressBuffer lists = ResourceDescriptorHeap[COV_LISTS];
        valid = index < min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
        if (valid) listEntry = lists.Load(4 * (list * capacity + index));
    }
    const CoverageDraw draw = coverageDraw(listEntry, valid, translucent);
    const bool hidden = valid && coverageDrawBehindCover(draw);
    if (hidden && lane == 0) state.InterlockedAdd(4 * VS_COV_CLUSTERS_TILE, 1);
    const bool drawn = valid && !hidden;
    const uint vertexCount = drawn ? clusterVertexCount(draw.cl) : 0, triangleCount = drawn ? clusterTriangleCount(draw.cl) : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[draw.cl.vertexOffset + i];
        const DeformedVertex dv = deformVertex(draw.inst, draw.mesh, meshVertex);
        const float4 p = viewModelClip(draw.inst, mul(draw.v.viewProj, float4(dv.world, 1)));  // A12: 1 outside the main view
        verts[i].position = p;
        gs_clip[i] = p;
        gs_uv[i] = loadVertex(draw.mesh, meshVertex).uv;
        gs_normal[i] = dv.normal;
        gs_world[i] = dv.world;
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    uint sent = 0, behindBandA = 0, behindCover = 0;
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[draw.cl.triangleOffset + t];
        const uint3 tri = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        tris[t] = tri;
        const float4 p[3] = { gs_clip[tri.x], gs_clip[tri.y], gs_clip[tri.z] };
        const float2 uv[3] = { gs_uv[tri.x], gs_uv[tri.y], gs_uv[tri.z] };
        const float3 nv[3] = { gs_normal[tri.x], gs_normal[tri.y], gs_normal[tri.z] };
        const float3 world[3] = { gs_world[tri.x], gs_world[tri.y], gs_world[tri.z] };
        const CoveragePrimitive cp = coveragePrimitive(draw, t, tri, p, uv, nv, world);
        const bool computed = ((taken[(t >> 5) & 3] >> (t & 31)) & 1u) != 0;  // (the compute rasteriser stored its fragments)
        PrimitiveOut o;
        o.visId = cp.visId;
        o.flags = cp.flags;
        o.material = cp.material;
        o.a = cp.a;
        o.b = cp.b;
        o.c = cp.c;
        o.d = cp.d;
        o.tab = cp.tab;
        o.tcd = cp.tcd;
        o.normals = cp.normals;
        o.cull = cp.cull || computed;
        prims[t] = o;
        if (!o.cull) ++sent;
        else if (!computed && cp.reason == COV_CULL_BAND_A) ++behindBandA;
        else if (!computed && cp.reason == COV_CULL_COVER) ++behindCover;
    }
    // The group's triangles by outcome (every lane is back here: one atomic per wave and nonzero counter).
    const uint sentSum = WaveActiveSum(sent), bandASum = WaveActiveSum(behindBandA), coverSum = WaveActiveSum(behindCover);
    if (WaveIsFirstLane())
    {
        if (sentSum > 0) state.InterlockedAdd(4 * VS_COV_TRIANGLES, sentSum);
        if (bandASum > 0) state.InterlockedAdd(4 * VS_COV_TRIANGLES_HIZ, bandASum);
        if (coverSum > 0) state.InterlockedAdd(4 * VS_COV_TRIANGLES_TILE, coverSum);
    }
}
