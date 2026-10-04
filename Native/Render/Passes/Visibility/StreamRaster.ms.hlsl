// unx-kernel: ms_6_6 main
// GPU triangle streams in the coverage layer (INTERFACES v1.60; W's water and fluid surfaces, request
// 20260926_W_gpu_triangle_stream): one mesh-shader group per 32 triangles of the stream's capacity; the live count is the
// draw arguments' vertex count / 3 (written on the GPU), groups past it emit nothing. Each triangle goes through the band B
// pixel kernel (CoverageRaster.ps) as a see-through record (no COV_FLAG_OPAQUE): exact area, 32-subsample mask, depth at
// the covered centroid, the interpolated vertex normal; band A keeps what lies beneath (the refraction reads it). Both
// faces are drawn (a water surface is seen from below too); the normal stays the authored one (M reads the triangle).
//   P[3].x vertices SRV (raw, 32 B per vertex), P[3].y draw arguments SRV (raw), P[3].z capacity (triangles),
//   P[3].w views SRV (the main view is element 0); P[6].x stream slot, P[6].y material; P[6].z waterVis SRV (UNX_NONE: a
//   coverage-layer stream; else a water-layer stream: records only in the layer's edge pixels, COV_FLAG_WATER_EDGE),
//   P[6].w waterDepth SRV, P[7].x band A depth SRV, P[7].y slot again (the pixel kernel's test); the rest as CoverageRaster.ms.
#include "Passes/Visibility/CoverageBuckets.hlsli"
#include "VisBuffer.hlsli"
#include "Passes/Common/TriangleStream.hlsli"

#define STREAM_VERTICES P[3].x
#define STREAM_ARGS P[3].y
#define STREAM_CAPACITY P[3].z
#define STREAM_SLOT P[6].x
#define STREAM_MATERIAL P[6].y

struct VertexOut
{
    float4 position : SV_Position;
};

struct PrimitiveOut  // CoverageRaster.ps's primitive inputs
{
    uint visId : VISID;
    uint flags : COVFLAGS;
    uint material : MATERIAL;
    float4 a : TRIA;
    float4 b : TRIB;
    float4 c : TRIC;
    float4 d : TRID;
    float4 tab : UVAB;
    float4 tcd : UVCD;
    uint4 normals : NRMS;
    bool cull : SV_CullPrimitive;
};

float4 toScreen(float4 p, float2 viewport)
{
    const float iw = 1.0 / p.w;
    return float4((p.x * iw * 0.5 + 0.5) * viewport.x, (0.5 - p.y * iw * 0.5) * viewport.y, p.z * iw, iw);
}

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[96], out primitives PrimitiveOut prims[32],
          out indices uint3 tris[32])
{
    ByteAddressBuffer args = ResourceDescriptorHeap[STREAM_ARGS];
    const uint live = min(args.Load(0) / 3, STREAM_CAPACITY);
    const uint first = (group.x + group.y * 65535) * 32;
    const uint count = first < live ? min(32u, live - first) : 0u;  // uniform over the group
    SetMeshOutputCounts(3 * count, count);
    if (lane >= count) return;
    const uint t = first + lane;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[COV_VIEWS];
    const CullView v = views[0];
    ByteAddressBuffer vertexData = ResourceDescriptorHeap[STREAM_VERTICES];
    float4 p[3];
    float3 n[3];
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const uint at = 32 * triangleStreamVertexIds(P[7].z, t)[k];
        p[k] = mul(v.viewProj, float4(asfloat(vertexData.Load3(at)), 1));
        n[k] = asfloat(vertexData.Load3(at + 16));
        verts[3 * lane + k].position = p[k];
    }
    tris[lane] = uint3(3 * lane, 3 * lane + 1, 3 * lane + 2);
    // Near clip (keep w - z >= 0), as CoverageRaster.ms.
    float4 q[4];
    float3 nq[4];
    uint m = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const uint j = k == 2 ? 0 : k + 1;
        const float ek = p[k].w - p[k].z, ej = p[j].w - p[j].z;
        if (ek >= 0)
        {
            q[m] = p[k];
            nq[m] = n[k];
            ++m;
        }
        if ((ek >= 0) != (ej >= 0))
        {
            const float s = ek / (ek - ej);
            q[m] = lerp(p[k], p[j], s);
            nq[m] = lerp(n[k], n[j], s);
            ++m;
        }
    }
    PrimitiveOut o = (PrimitiveOut)0;
    o.visId = COV_STREAM_ID | (STREAM_SLOT << 24) | t;
    o.material = STREAM_MATERIAL;
    bool cull = m < 3;
    if (!cull)
    {
        if (m == 3)
        {
            q[3] = q[2];
            nq[3] = nq[2];
        }
        o.a = toScreen(q[0], v.viewportSize);
        o.b = toScreen(q[1], v.viewportSize);
        o.c = toScreen(q[2], v.viewportSize);
        o.d = toScreen(q[3], v.viewportSize);
        const float area2 = (o.a.x * o.b.y - o.b.x * o.a.y) + (o.b.x * o.c.y - o.c.x * o.b.y) + (o.c.x * o.d.y - o.d.x * o.c.y) + (o.d.x * o.a.y - o.a.x * o.d.y);
        o.flags = (m == 4 ? COV_FLAG_QUAD : 0u) | (P[6].z != UNX_NONE ? COV_FLAG_WATER_EDGE : 0u);  // see-through: no COV_FLAG_OPAQUE
        cull = area2 == 0;
        if (!cull && COV_RASTER_TRIANGLE_CULL)
        {
            // Reuse band A's existing HiZ. The pixel kernel clamps depth to this
            // polygon's vertex range and tests the same one-pixel neighbourhood,
            // so these triangles could not have produced a surviving record.
            const float2 lo = min(min(o.a.xy, o.b.xy), min(o.c.xy, o.d.xy));
            const float2 hi = max(max(o.a.xy, o.b.xy), max(o.c.xy, o.d.xy));
            const float nearest = max(max(o.a.z, o.b.z), max(o.c.z, o.d.z));
            cull = hi.x <= 0 || hi.y <= 0 || lo.x >= (float)COV_WIDTH || lo.y >= (float)COV_HEIGHT;
            if (!cull) cull = coverageRectBehindBandA(lo, hi, nearest);
        }
        if (!cull) o.normals = uint4(coverageOct32(nq[0]), coverageOct32(nq[1]), coverageOct32(nq[2]), coverageOct32(nq[3]));
    }
    o.cull = cull;
    prims[lane] = o;
}
