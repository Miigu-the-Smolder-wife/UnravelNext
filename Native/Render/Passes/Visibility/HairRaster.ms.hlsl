// unx-kernel: ms_6_6 main
// Strand hair in the coverage layer (band B; E's request 20260926_E_hair_strands, B10; INTERFACES v1.59): one mesh-shader
// group per 32 segments of FrameResources::hairSegments (camera-relative (p0, r0), (p1, r1); E writes the segments of
// the strands its LOD keeps, so the groups are as many as what is drawn; r = 0: not drawn).
// Each lane turns its segment into the ribbon of width 2r facing the camera (the capsule's silhouette without its caps:
// consecutive segments share their end points bit for bit), two triangles through the band B pixel kernel
// (CoverageRaster.ps): per pixel the exact area of the ribbon triangle inside it, the 32-subsample mask, the depth at the
// covered region's centroid. The record's vis id is COV_HAIR_ID | segment (CoverageTiles.hlsli coverageFragmentHair*), its
// normal bits carry the strand coordinate u (root 0, tip 1; M rebuilds the tangent from p1 - p0). Conditions: at a bend
// the ribbons of two segments leave a wedge gap outside and overlap inside of about w^2 x angle / 2; a segment pointing at
// the camera (ribbon side undefined, length < 1e-6 of its distance) is left out.
//   P[3].x hair segments SRV (StructuredBuffer<float4>), P[3].y bodies SRV (raw), P[3].z segment count, P[3].w views SRV
//   (the main view is element 0); the rest as CoverageRaster.ms (CoverageLayer.hlsli).
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "VisBuffer.hlsli"

#define HAIR_SEGMENTS P[3].x
#define HAIR_BODIES P[3].y
#define HAIR_COUNT P[3].z
#define HAIR_BODY_WORDS 8u
#define HAIR_BODIES_MAX 4096u  // bounded body search (the header's count is clamped; an overflow draws nothing for it)

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
    uint4 normals : NRMS;  // hair: asuint(u) of a, b, c, d
    bool cull : SV_CullPrimitive;
};

float4 toScreen(float4 p, float2 viewport)
{
    const float iw = 1.0 / p.w;
    return float4((p.x * iw * 0.5 + 0.5) * viewport.x, (0.5 - p.y * iw * 0.5) * viewport.y, p.z * iw, iw);
}

// One ribbon triangle, near-clipped (keep w - z >= 0) as CoverageRaster.ms does; u rides along like the uvs there.
PrimitiveOut ribbonTriangle(CullView v, float4 p0, float4 p1, float4 p2, float u0, float u1, float u2, uint visId, uint material, bool live)
{
    const float4 p[3] = { p0, p1, p2 };
    const float uu[3] = { u0, u1, u2 };
    float4 q[4];
    float qu[4];
    uint n = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const uint j = k == 2 ? 0 : k + 1;
        const float ek = p[k].w - p[k].z, ej = p[j].w - p[j].z;
        if (ek >= 0)
        {
            q[n] = p[k];
            qu[n] = uu[k];
            ++n;
        }
        if ((ek >= 0) != (ej >= 0))
        {
            const float s = ek / (ek - ej);
            q[n] = lerp(p[k], p[j], s);
            qu[n] = lerp(uu[k], uu[j], s);
            ++n;
        }
    }
    PrimitiveOut o = (PrimitiveOut)0;
    o.visId = visId;
    o.material = material;
    bool cull = !live || n < 3;
    if (!cull)
    {
        if (n == 3)
        {
            q[3] = q[2];
            qu[3] = qu[2];
        }
        o.a = toScreen(q[0], v.viewportSize);
        o.b = toScreen(q[1], v.viewportSize);
        o.c = toScreen(q[2], v.viewportSize);
        o.d = toScreen(q[3], v.viewportSize);
        o.normals = uint4(asuint(qu[0]), asuint(qu[1]), asuint(qu[2]), asuint(qu[3]));
        const float area2 = (o.a.x * o.b.y - o.b.x * o.a.y) + (o.b.x * o.c.y - o.c.x * o.b.y) + (o.c.x * o.d.y - o.d.x * o.c.y) + (o.d.x * o.a.y - o.a.x * o.d.y);
        o.flags = COV_FLAG_HAIR | COV_FLAG_OPAQUE | (n == 4 ? COV_FLAG_QUAD : 0u);  // a strand is opaque; both faces drawn
        cull = area2 == 0;
    }
    o.cull = cull;
    return o;
}

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[64],
          out indices uint3 tris[64])
{
    const uint first = (group.x + group.y * 65535) * 32;
    const uint count = first < HAIR_COUNT ? min(32u, HAIR_COUNT - first) : 0u;  // uniform over the group
    SetMeshOutputCounts(4 * count, 2 * count);
    if (lane >= count) return;
    const uint s = first + lane;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[COV_VIEWS];
    const CullView v = views[0];
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[HAIR_SEGMENTS];
    ByteAddressBuffer bodies = ResourceDescriptorHeap[HAIR_BODIES];
    const float4 a = segments[2 * s], b = segments[2 * s + 1];
    // The body holding segment s: its material, and u from the segment's place in its strand.
    const uint bodyCount = min(bodies.Load(0), HAIR_BODIES_MAX);
    uint material = 0, perStrand = 1, bodyFirst = 0;
    bool found = false;
    for (uint k = 0; k < bodyCount && !found; ++k)
    {
        const uint4 h = bodies.Load4(4 + 4 * HAIR_BODY_WORDS * k);  // first segment, segments, segments per strand, material
        if (s >= h.x && s - h.x < h.y)
        {
            found = true;
            bodyFirst = h.x;
            perStrand = max(h.z, 1u);
            material = h.w;
        }
    }
    const uint i = (s - bodyFirst) % perStrand;
    const float u0 = (float)i / perStrand, u1 = (float)(i + 1) / perStrand;
    // Ribbon facing the camera: across the segment and the view ray to its middle (camera-relative positions).
    const float3 dir = b.xyz - a.xyz, mid = 0.5 * (a.xyz + b.xyz);
    float3 side = cross(dir, mid);
    const float sideLength = length(side);
    const bool live = found && a.w > 0 && b.w > 0 && sideLength > 1e-6 * length(dir) * length(mid);
    side = live ? side / sideLength : float3(0, 0, 0);
    const float3 corner[4] = { a.xyz - side * a.w, a.xyz + side * a.w, b.xyz + side * b.w, b.xyz - side * b.w };
    float4 clip[4];
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        clip[k] = mul(v.viewProj, float4(corner[k] + v.position, 1));
        verts[4 * lane + k].position = clip[k];
    }
    const uint visId = COV_HAIR_ID | s;
    tris[2 * lane] = uint3(4 * lane, 4 * lane + 1, 4 * lane + 2);
    tris[2 * lane + 1] = uint3(4 * lane, 4 * lane + 2, 4 * lane + 3);
    prims[2 * lane] = ribbonTriangle(v, clip[0], clip[1], clip[2], u0, u0, u1, visId, material, live);
    prims[2 * lane + 1] = ribbonTriangle(v, clip[0], clip[2], clip[3], u0, u1, u1, visId, material, live);
}
