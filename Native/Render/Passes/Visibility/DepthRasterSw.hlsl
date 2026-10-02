// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// Software rasteriser of a raster request with a tile atlas (visibility.software_raster; S's shadow pages. The reference's
// NaniteRasterizer.usf compute path into its virtual shadow map pages). The atlas is a depth target (no storage access),
// so the compute rasteriser has pages of its own - one per set tile of the request's views, tilePx x tilePx words of
// depth bits, the nearest surface the largest (reversed Z; positive floats order as their bits) - and DepthSwMerge
// draws each page into its tile's atlas slot under the depth test at the end of the request's raster pass: the atlas
// holds both rasterisers' result before any reader (the requester's passes after the request).
// Which clusters: CullClusters' softwareCluster (small in the view, rigid or wind-deformed, every set tile under them
// with a page); they are the run's LIST_SW and the mesh raster does not see them. The request draws depth only (no pixel
// kernel: alpha-tested materials are solid in it, as in its mesh raster) and both faces.
//   MODE=0 begin (1 thread): the arguments and the tile count emptied.
//   MODE=1 pages (64 mask words per group, a row of groups per view): every tile of the views gets its word in the
//          page map - SW_PAGE_NONE when it is not set, else the next page, SW_PAGE_FULL past the capacity - and a page
//          remembers its tile's atlas slot. Before the cull (CullClusters reads the map).
//   MODE=2 clear (one group per page, 64 threads, tilePx^2 / 64 stores each: <= 1024 with tiles of at most 256 px): the
//          page's words to 0 (no surface). The first group stores the tile count in the run's state (VS_SW_TILES).
//   MODE=3 raster: one group of 64 threads per LIST_SW entry. The cluster's vertices through deformVertex as the mesh
//          kernel's, to view pixels; a thread per triangle sets it up (RasterSw.hlsli: the hardware's coverage for the same
//          vertices - the mesh kernel's whole-pixel shift to the slot leaves the pixel centres where they are) and
//          walks its rectangle; a pixel in a tile with a page writes its depth there by atomic max.
// Work per raster group: the cluster's vertices (<= 128), two rounds of 64 triangles, each at most SW_MAX_SPAN^2 pixels.
//   MODE 0, 1:
//   P[0] views SRV, view count, tile mask SRV (raw: DepthRasterRequest::cullMask), atlas slots SRV (raw)
//   P[1] software arguments UAV (raw: DSA_*), page map UAV (raw), page slots UAV (raw: DSP_*), page capacity
//   MODE 2, 3:
//   P[0] visible SRV (uint2), lists SRV (raw), state UAV (raw), list (LIST_SW)
//   P[1] phase (1), list capacity, views SRV, page map SRV (raw)
//   P[2] page depth UAV (raw: page x tilePx^2 words), page slots SRV (raw), page capacity, tile px
#include "Passes/Visibility/VisibilityCommon.hlsli"
#include "Passes/Visibility/RasterSw.hlsli"
#include "Passes/Visibility/DepthRasterSw.hlsli"

#if MODE == 0
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[1].z];
    args.Store3(4 * DSA_CLEAR, uint3(0, 1, 1));
    args.Store3(4 * DSA_MERGE, uint3(0, 1, 1));
    args.Store2(4 * 6, uint2(0, 0));
    slots.Store(4 * DSP_TILES, 0);
}

#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint view = id.y, w = id.x, capacity = P[1].w;
    if (view >= P[0].y) return;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[0].x];
    const CullView v = views[view];
    if (v.cullMaskOffset == UNX_NONE) return;
    const uint tiles = v.tilesX * (((uint)v.viewportSize.y + v.tilePx - 1) / v.tilePx);
    if (w * 32 >= tiles) return;
    ByteAddressBuffer mask = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer atlasSlots = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer pages = ResourceDescriptorHeap[P[1].y];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[1].z];
    const uint here = min(tiles - w * 32, 32u);  // the word's tiles (a view's mask starts at a word)
    const uint bits = mask.Load(4 * (v.cullMaskOffset + w)) & (here == 32 ? 0xFFFFFFFFu : (1u << here) - 1);
    const uint n = countbits(bits);
    uint base = 0;
    if (n > 0)
    {
        slots.InterlockedAdd(4 * DSP_TILES, n, base);
        const uint kept = min(base + n, capacity);
        // (the clear's and the merge's arguments follow the pages given out, as the cull kernels' raiseDispatch)
        args.InterlockedMax(4 * DSA_CLEAR, kept);
        args.InterlockedMax(4 * DSA_MERGE, (kept + DSW_MERGE_PAGES - 1) / DSW_MERGE_PAGES);
    }
    uint taken = 0;
    for (uint i = 0; i < here; ++i)  // <= 32
    {
        const uint word = (v.cullMaskOffset + w) * 32 + i;
        uint page = SW_PAGE_NONE;
        if ((bits >> i) & 1u)
        {
            page = base + taken < capacity ? base + taken : SW_PAGE_FULL;
            ++taken;
            if (page != SW_PAGE_FULL) slots.Store(4 * (DSP_FIRST + page), atlasSlots.Load(4 * word));
        }
        pages.Store(4 * word, page);
    }
}

#elif MODE == 2
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    RWByteAddressBuffer target = ResourceDescriptorHeap[P[2].x];
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[2].y];
    const uint page = group.x, texels = P[2].w * P[2].w;
    if (page >= P[2].z) return;  // (the arguments never pass the capacity)
    for (uint i = lane; i < texels; i += 64) target.Store(4 * (page * texels + i), 0);
    if (page == 0 && lane == 0)
    {
        RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
        state.Store(4 * VS_SW_TILES, slots.Load(4 * DSP_TILES));
    }
}

#else
groupshared float3 gs_pixel[128];  // x, y in view pixels, device depth

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint index = group.x + group.y * 65535;
    if (index >= count) return;  // uniform over the group
    const uint visibleIndex = lists.Load(4 * (list * capacity + index));
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = visible[visibleIndex];
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[entry.y >> 24];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint vertexCount = min(clusterVertexCount(cl), 128u), triangleCount = min(clusterTriangleCount(cl), 128u);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const DeformedVertex d = deformVertex(inst, mesh, clusterVertices[cl.vertexOffset + i]);
        const float4 p = mul(v.viewProj, float4(d.world, 1));
        const float iw = 1.0 / p.w;  // (the classification: the cluster's sphere lies in front of the near plane)
        gs_pixel[i] = float3((p.x * iw * 0.5 + 0.5) * v.viewportSize.x, (0.5 - p.y * iw * 0.5) * v.viewportSize.y, p.z * iw);
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    RWByteAddressBuffer target = ResourceDescriptorHeap[P[2].x];
    ByteAddressBuffer pages = ResourceDescriptorHeap[P[1].w];
    const uint pageCapacity = P[2].z, tilePx = v.tilePx, texels = tilePx * tilePx;
    const int2 size = int2(v.viewportSize);
    uint drawn = 0;
    bool cut = false;
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        const SwTriangle tri = swSetup(gs_pixel[packed & 0xFFu], gs_pixel[(packed >> 8) & 0xFFu], gs_pixel[(packed >> 16) & 0xFFu], 0, int2(0, 0), size - 1);
        cut = cut || tri.cut;
        if (!tri.valid) continue;
        ++drawn;
        int3 row = tri.edge;
        for (int y = tri.lo.y; y <= tri.hi.y; ++y)
        {
            int3 e = row;
            for (int x = tri.lo.x; x <= tri.hi.x; ++x)
            {
                if (min(e.x, min(e.y, e.z)) >= 0)
                {
                    const float depth = swDepth(tri, e);
                    // (depth 0 is "nothing"; past the near plane cannot be: the cluster's sphere lies behind it)
                    if (depth > 0)
                    {
                        const uint2 pixel = uint2(x, y), tile = pixel / tilePx, local = pixel - tile * tilePx;
                        const uint page = pages.Load(4 * (v.cullMaskOffset * 32 + tile.y * v.tilesX + tile.x));
                        if (page < pageCapacity) target.InterlockedMax(4 * (page * texels + local.y * tilePx + local.x), asuint(min(depth, 1.0)));
                    }
                }
                e += tri.stepX;
            }
            row += tri.stepY;
        }
    }
    // The group's statistics (every lane is back here), and a triangle past the loop bound (a defect of the bound).
    const uint drawnSum = WaveActiveSum(drawn);
    const bool cutAny = WaveActiveAnyTrue(cut);
    if (WaveIsFirstLane())
    {
        if (drawnSum > 0) state.InterlockedAdd(4 * VS_SW_TRIANGLES, drawnSum);
        if (cutAny) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);
    }
    if (lane == 0) state.InterlockedAdd(4 * VS_SW_CLUSTERS, 1);
}
#endif
