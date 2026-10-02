// unx-kernel: ms_6_6 main
// unx-variants: TILE=0,1,2 DEPTH=0,1 OUT=64,128 AS=0,1
// Depth raster service (FrameServices::rasterizeDepth, INTERFACES 5.3): one mesh-shader group per draw-list entry,
// any number of views (the visible entry carries the view). Outputs match struct DepthRasterPixel (DepthRaster.hlsli)
// for the requester's pixel kernel: position, uv (alpha test), userData, material, instance.
// Views with different viewports select theirs with SV_ViewportArrayIndex (at most 16 per request).
// TILE=1 (DepthRasterRequest::tileLocal): the group is one (cluster, tile rectangle) pair launched by DepthRaster.as.hlsl
// (DepthRasterPayload.hlsli: the pair from the payload's per-row prefix). Triangles outside the rectangle are culled
// and the rest are clipped to it by four clip distances, so the rasteriser makes fragments only inside the requested
// tiles; positions are unchanged (same pixels, same depth).
// TILE=2 (atlas mode, DepthRasterRequest::atlasSlots): pairs are single tiles (CULL_VIEW_TILE_SINGLE); the tile moves to
// its atlas slot by a whole-pixel shift in clip space, the viewport being the whole atlas.
// DEPTH=1 (no pixel kernel: hardware depth only): only the position, the clip distances, the viewport and the cull flag
// are exported. The attributes a pixel kernel reads (uv, userData, material, instance) are dead there, and each mesh
// shader group's output size limits how many groups an SM holds at once.
//   P[0] visible SRV (uint2), lists SRV (raw), state SRV (raw), list
//   P[1] phase (always 1: the service culls in one phase), list capacity, views SRV, viewport per view (0 = one viewport)
//   P[2] tile rectangles SRV (TILE=1,2: read by the amplification stage), atlas slots SRV (raw, TILE=2), atlas tiles per
//        row, atlas size (w | h << 16)
//   P[3] tile mask SRV (raw, TILE=1,2), the frame's cluster page table's SRV + 1 (visibility.cluster_streaming; 0: none)
// AS=0 with TILE (visibility.raster_amplification false, A/B): the group is a draw-list entry of the stored pair list
// (P[2].x: uint3 (visible index, tile rectangle) per pair), dispatched directly.
#include "Passes/Visibility/VisibilityCommon.hlsli"
#define FROM_PAYLOAD (TILE && AS)
#if FROM_PAYLOAD
#include "Passes/Visibility/DepthRasterPayload.hlsli"
#endif

// OUT: the declared output arrays (vertices, primitives). The service picks 64 when every installed cluster has at most
// 64 vertices and 64 triangles (GpuScene::maxClusterVertices/Triangles; visibility.cluster_vertices/triangles = 64): the
// group's output allocation halves, so an SM holds more groups (the raster is bound by that, not by the culled triangles).
#define MS_OUT OUT

struct VertexOut
{
    float4 position : SV_Position;
#if !DEPTH
    float2 uv : TEXCOORD0;
#endif
#if TILE
    float4 clip : SV_ClipDistance0;  // >= 0 inside the pair's tile rectangle (left, right, top, bottom)
#endif
};

struct PrimitiveOut
{
#if !DEPTH
    uint userData : USERDATA;
    uint material : MATERIAL;
    uint instance : INSTANCE;
#endif
#if TILE != 2
    uint viewport : SV_ViewportArrayIndex;  // (atlas mode: every view's viewport is the whole atlas, so none is exported)
#endif
    bool cull : SV_CullPrimitive;  // tile rectangle (TILE), C5 terrain patch blocks
};

#if TILE
groupshared float3 g_pixel[MS_OUT];  // viewport-relative pixel position per vertex, z = 1 in front of the eye
#endif

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[MS_OUT], out primitives PrimitiveOut prims[MS_OUT],
          out indices uint3 tris[MS_OUT]
#if FROM_PAYLOAD
          , in payload DepthRasterPayload payload
#endif
)
{
#if FROM_PAYLOAD
    const uint pairIndex = group.x + group.y * DR_GRID;
    const bool valid = pairIndex < payload.total;  // uniform over the group (the grid rounds up to whole rows)
    const uint visibleIndex = payload.visibleIndex;
#else
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint index = group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint entryIndex = valid ? lists.Load(4 * (list * capacity + index)) : 0;
#if TILE
    StructuredBuffer<uint3> storedPairs = ResourceDescriptorHeap[P[2].x];
    const uint3 storedPair = valid ? storedPairs[entryIndex] : uint3(0, 0, 0);
    const uint visibleIndex = storedPair.x;
#else
    const uint visibleIndex = entryIndex;
#endif
#endif
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = valid ? visible[visibleIndex] : uint2(0, 0);
    const uint view = entry.y >> 24;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[view];
#if TILE && !FROM_PAYLOAD
    const uint3 pair = storedPair;
#elif FROM_PAYLOAD
    // This group's pair: the whole rectangle, or the row whose prefix range holds pairIndex (binary search over the
    // non-decreasing prefix) and that row's (pairIndex - prefix)-th run or tile.
    uint3 pair = uint3(visibleIndex, payload.rectLo, payload.rectHi);
    if (valid && payload.whole == 0)
    {
        const uint2 a = uint2(payload.rectLo & 0xFFFFu, payload.rectLo >> 16), b = uint2(payload.rectHi & 0xFFFFu, payload.rectHi >> 16);
        const uint rows = min(b.y - a.y + 1, DR_MAX_ROWS);
        uint lo = 0, hi = rows - 1;  // the last row r with prefix[r] <= pairIndex
        while (lo < hi)
        {
            const uint mid = (lo + hi + 1) >> 1;
            if (payload.prefix[mid] <= pairIndex) lo = mid;
            else hi = mid - 1;
        }
        ByteAddressBuffer mask = ResourceDescriptorHeap[P[3].x];
        const uint y = a.y + lo;
        const uint2 run = drRowPair(mask, v.cullMaskOffset, v.tilesX, y, a.x, b.x, (v.flags & CULL_VIEW_TILE_SINGLE) != 0, pairIndex - payload.prefix[lo]);
        pair.y = run.x | (y << 16);
        pair.z = run.y | (y << 16);
    }
#endif
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint material = clusterMaterial(inst, cl);
    // (the CPU picks OUT64 only when every cluster fits; the clamp only keeps an output count within the declaration)
    const uint vertexCount = valid ? min(clusterVertexCount(cl), MS_OUT) : 0, triangleCount = valid ? min(clusterTriangleCount(cl), MS_OUT) : 0;
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
    // (visibility.cluster_compression: the cluster's own vertices, in the stream or in its resident page)
    const ClusterVertexSource vertexSource = clusterVertexSource(entry.y & 0xFFFFFFu, P[3].y);
    for (uint i = lane; i < vertexCount; i += 64)
    {
        VertexData vertex;
        const DeformedVertex d = deformClusterVertex(inst, mesh, cl, vertexSource, i, vertex);
        const float4 p = mul(v.viewProj, float4(d.world, 1));
#if TILE == 2
        verts[i].position = float4(p.x * scale.x + p.w * offset.x, p.y * scale.y + p.w * offset.y, p.z, p.w);
#else
        verts[i].position = p;
#endif
#if !DEPTH
        verts[i].uv = vertex.uv;
#endif
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
#if !DEPTH
        prims[t].userData = v.userData;
        prims[t].material = material;
        prims[t].instance = entry.x;
#endif
#if TILE != 2
        prims[t].viewport = P[1].w != 0 ? view : 0;
#endif
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
