// unx-kernel: ms_6_6 main
// unx-variants: TILE=0,1,2
// Depth raster service (FrameServices::rasterizeDepth, INTERFACES 5.3): one mesh-shader group per draw-list entry,
// any number of views (the visible entry carries the view). Outputs match struct DepthRasterPixel (DepthRaster.hlsli)
// for the requester's pixel kernel: position, uv (alpha test), userData, material, instance.
// Views with different viewports select theirs with SV_ViewportArrayIndex (at most 16 per request).
// TILE=1 (DepthRasterRequest::tileLocal): the entry is a (cluster, tile rectangle) pair (CullClusters,
// writeTilePairs). Triangles outside the rectangle are culled and the rest are clipped to it by four clip distances,
// so the rasteriser makes fragments only inside the requested tiles; positions are unchanged (same pixels, same depth).
// TILE=2 (atlas mode, DepthRasterRequest::atlasSlots): pairs are single tiles (CULL_VIEW_TILE_SINGLE); the tile moves to
// its atlas slot by a whole-pixel shift in clip space, the viewport being the whole atlas.
//   P[0] visible SRV (uint2), lists SRV (raw), state SRV (raw), list
//   P[1] phase (always 1: the service culls in one phase), list capacity, views SRV, viewport per view (0 = one viewport)
//   P[2] tile pairs SRV (uint3, TILE=1,2), atlas slots SRV (raw, TILE=2), atlas tiles per row, atlas size (w | h << 16)
#include "Passes/Visibility/VisibilityCommon.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
#if TILE
    float4 clip : SV_ClipDistance0;  // >= 0 inside the pair's tile rectangle (left, right, top, bottom)
#endif
};

struct PrimitiveOut
{
    uint userData : USERDATA;
    uint material : MATERIAL;
    uint instance : INSTANCE;
    uint viewport : SV_ViewportArrayIndex;
    bool cull : SV_CullPrimitive;  // tile rectangle (TILE), C5 terrain patch blocks
};

#if TILE
groupshared float3 g_pixel[128];  // viewport-relative pixel position per vertex, z = 1 in front of the eye
#endif

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[128],
          out indices uint3 tris[128])
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint index = group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint entryIndex = valid ? lists.Load(4 * (list * capacity + index)) : 0;
#if TILE
    StructuredBuffer<uint3> pairs = ResourceDescriptorHeap[P[2].x];
    const uint3 pair = valid ? pairs[entryIndex] : uint3(0, 0, 0);
    const uint visibleIndex = pair.x;
#else
    const uint visibleIndex = entryIndex;
#endif
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = valid ? visible[visibleIndex] : uint2(0, 0);
    const uint view = entry.y >> 24;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[view];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint material = clusterMaterial(inst, cl);
    const uint vertexCount = valid ? clusterVertexCount(cl) : 0, triangleCount = valid ? clusterTriangleCount(cl) : 0;
#if TILE
    // Tile rectangle in viewport pixels [lo, hi) and as NDC bounds (y up: top = 1 - 2 lo.y / H).
    const float2 lo = float2(pair.y & 0xFFFFu, pair.y >> 16) * v.tilePx;
    const float2 hi = min(float2((pair.z & 0xFFFFu) + 1, (pair.z >> 16) + 1) * v.tilePx, v.viewportSize);
    const float2 ndcLo = float2(2 * lo.x / v.viewportSize.x - 1, 1 - 2 * lo.y / v.viewportSize.y);
    const float2 ndcHi = float2(2 * hi.x / v.viewportSize.x - 1, 1 - 2 * hi.y / v.viewportSize.y);
#endif
#if TILE == 2
    // Tile -> slot by the whole-pixel shift d: x' = x s.x + w (s.x - 1 + 2 d.x / A.x), y' = y s.y + w (1 - s.y - 2 d.y /
    // A.y) with s = viewport / atlas size; z and w unchanged (same depth, same perspective). The clip distances stay in
    // the view's clip space: they cut the same tile edges.
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[2].y];
    const uint tile = (pair.y >> 16) * v.tilesX + (pair.y & 0xFFFFu);
    const uint slot = valid ? slots.Load(4 * (v.cullMaskOffset * 32 + tile)) : 0;
    const float2 atlasSize = float2(P[2].w & 0xFFFFu, P[2].w >> 16);
    const float2 shift = float2(slot % P[2].z, slot / P[2].z) * v.tilePx - lo;
    const float2 scale = v.viewportSize / atlasSize;
    const float2 offset = float2(scale.x - 1 + 2 * shift.x / atlasSize.x, 1 - scale.y - 2 * shift.y / atlasSize.y);
#endif
    SetMeshOutputCounts(vertexCount, triangleCount);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[cl.vertexOffset + i];
        const DeformedVertex d = deformVertex(inst, mesh, meshVertex);
        const float4 p = mul(v.viewProj, float4(d.world, 1));
#if TILE == 2
        verts[i].position = float4(p.x * scale.x + p.w * offset.x, p.y * scale.y + p.w * offset.y, p.z, p.w);
#else
        verts[i].position = p;
#endif
        verts[i].uv = loadVertex(mesh, meshVertex).uv;
#if TILE
        verts[i].clip = float4(p.x - ndcLo.x * p.w, ndcHi.x * p.w - p.x, ndcLo.y * p.w - p.y, p.y - ndcHi.y * p.w);
        g_pixel[i] = p.w > 0 ? float3((p.x / p.w * 0.5 + 0.5) * v.viewportSize.x, (0.5 - p.y / p.w * 0.5) * v.viewportSize.y, 1) : 0;
#endif
    }
#if TILE
    GroupMemoryBarrierWithGroupSync();
#endif
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        const uint3 tri = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        tris[t] = tri;
        prims[t].userData = v.userData;
        prims[t].material = material;
        prims[t].instance = entry.x;
        prims[t].viewport = P[1].w != 0 ? view : 0;
#if TILE
        // Outside the rectangle: its pixel box misses [lo, hi] (triangles reaching behind the eye are kept).
        const float3 a = g_pixel[tri.x], b = g_pixel[tri.y], c = g_pixel[tri.z];
        const float2 boxLo = min(a.xy, min(b.xy, c.xy)), boxHi = max(a.xy, max(b.xy, c.xy));
        const bool outside = a.z * b.z * c.z > 0 && (boxHi.x < lo.x || boxLo.x > hi.x || boxHi.y < lo.y || boxLo.y > hi.y);
        prims[t].cull = outside || patchDropsTriangle(inst, mesh, cl, tri);  // C5: replaced terrain blocks
#else
        prims[t].cull = patchDropsTriangle(inst, mesh, cl, tri);  // C5
#endif
    }
}
