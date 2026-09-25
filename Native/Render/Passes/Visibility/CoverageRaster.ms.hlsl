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
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "VisBuffer.hlsli"

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

float4 toScreen(float4 p, float2 viewport)
{
    const float iw = 1.0 / p.w;
    return float4((p.x * iw * 0.5 + 0.5) * viewport.x, (0.5 - p.y * iw * 0.5) * viewport.y, p.z * iw, iw);
}

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[128],
          out indices uint3 tris[128])
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];  // the pass writes it (pixel kernel counters)
    ByteAddressBuffer lists = ResourceDescriptorHeap[COV_LISTS];
    const uint capacity = COV_LIST_CAPACITY;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), capacity);
    const uint index = group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint visibleIndex = valid ? lists.Load(4 * (LIST_B * capacity + index)) : 0;
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[COV_VISIBLE];
    const uint2 entry = valid ? visible[visibleIndex] : uint2(0, 0);
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[COV_VIEWS];
    const CullView v = views[entry.y >> 24];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint material = clusterMaterial(inst, cl);
    const GpuMaterial m = loadMaterial(material);
    const bool oneSided = (m.classFlags & MATERIAL_TWO_SIDED) == 0 && (v.flags & CULL_VIEW_CULL_BACK) != 0;
    const uint alphaFlag = (m.classFlags & MATERIAL_ALPHA_TESTED) != 0 ? COV_FLAG_ALPHA : 0u;
    // Opaque for the view: not a see-through class (a leaf's transmission is light, not view; an alpha-tested fragment
    // hides what its mask covers after the test).
    const uint materialClass = m.classFlags & 0xFFu;
    const uint opaqueFlag = materialClass != MATERIAL_GLASS && materialClass != MATERIAL_WATER ? COV_FLAG_OPAQUE : 0u;
    const uint vertexCount = valid ? clusterVertexCount(cl) : 0, triangleCount = valid ? clusterTriangleCount(cl) : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[cl.vertexOffset + i];
        const DeformedVertex dv = deformVertex(inst, mesh, meshVertex);
        const float4 p = mul(v.viewProj, float4(dv.world, 1));
        verts[i].position = p;
        gs_clip[i] = p;
        gs_uv[i] = loadVertex(mesh, meshVertex).uv;
        gs_normal[i] = dv.normal;
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        const uint3 tri = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        tris[t] = tri;
        const float4 p[3] = { gs_clip[tri.x], gs_clip[tri.y], gs_clip[tri.z] };
        const float2 uv[3] = { gs_uv[tri.x], gs_uv[tri.y], gs_uv[tri.z] };
        const float3 nv[3] = { gs_normal[tri.x], gs_normal[tri.y], gs_normal[tri.z] };
        // Near clip (keep w - z >= 0): Sutherland-Hodgman against one plane leaves 3 or 4 vertices (0 when behind).
        // Clip-space interpolation is linear in the world position, so the uvs interpolate with the same parameter.
        float4 q[4];
        float2 tq[4];
        float3 nq[4];
        uint n = 0;
        [unroll] for (uint k = 0; k < 3; ++k)
        {
            const uint j = k == 2 ? 0 : k + 1;
            const float ek = p[k].w - p[k].z, ej = p[j].w - p[j].z;
            if (ek >= 0)
            {
                q[n] = p[k];
                tq[n] = uv[k];
                nq[n] = nv[k];
                ++n;
            }
            if ((ek >= 0) != (ej >= 0))
            {
                const float s = ek / (ek - ej);
                q[n] = lerp(p[k], p[j], s);
                tq[n] = lerp(uv[k], uv[j], s);
                nq[n] = lerp(nv[k], nv[j], s);
                ++n;
            }
        }
        PrimitiveOut o = (PrimitiveOut)0;
        o.visId = packVisId(visibleIndex, t);
        o.material = material;
        bool cull = n < 3;
        if (!cull)
        {
            if (n == 3)
            {
                q[3] = q[2];
                tq[3] = tq[2];
                nq[3] = nq[2];
            }
            o.a = toScreen(q[0], v.viewportSize);
            o.b = toScreen(q[1], v.viewportSize);
            o.c = toScreen(q[2], v.viewportSize);
            o.d = toScreen(q[3], v.viewportSize);
            o.tab = float4(tq[0], tq[1]);
            o.tcd = float4(tq[2], tq[3]);
            o.normals = uint4(coverageOct32(nq[0]), coverageOct32(nq[1]), coverageOct32(nq[2]), coverageOct32(nq[3]));
            // Signed area of the polygon (shoelace), y-down pixels.
            const float area2 = (o.a.x * o.b.y - o.b.x * o.a.y) + (o.b.x * o.c.y - o.c.x * o.b.y) + (o.c.x * o.d.y - o.d.x * o.c.y) + (o.d.x * o.a.y - o.a.x * o.d.y);
            const bool back = COV_FRONT_SIGN * area2 > 0;
            o.flags = alphaFlag | opaqueFlag | (n == 4 ? COV_FLAG_QUAD : 0u) | (back ? COV_FLAG_BACK : 0u);
            cull = area2 == 0 || (oneSided && back);
        }
        o.cull = cull;
        prims[t] = o;
    }
}
