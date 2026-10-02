// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Software rasteriser of the main view's visibility buffer (visibility.software_raster; the reference's
// NaniteRasterizer.usf compute path with NaniteWritePixel.ush): the band A clusters RasterBins.hlsl gave this path -
// small on screen, no alpha test, rigid or wind-deformed - are rasterised by compute into a 64-bit word per pixel,
// depth bits << 32 | vis id, by atomic max (reversed Z: the nearest surface is the largest word; equal depths keep the
// larger id). VisSwResolve.ps then writes those pixels into the depth buffer and the vis id target under the depth
// test, so every reader of depth, vis id and HiZ sees one result. The hardware raster's pixel kernel does not write the
// 64-bit word (the reference's does): here the mesh path keeps its early depth test and its targets, and the joining
// pass costs one 8 B load per pixel.
//   MODE=0 clear (64 words per group): the frame's words to 0 (no surface).
//   MODE=1 raster: one group of 64 threads per software entry of a list (the entries after the list's hardware ones in
//          the sorted lists). The cluster's vertices through deformVertex as the mesh kernel's, to target pixels; a
//          thread per triangle sets it up (RasterSw.hlsli: 1 / 256 px snap, top-left rule, pixel centres - the
//          hardware's coverage for the same vertices) and walks its rectangle. A pixel behind the depth buffer as it
//          stands (the hardware raster of this phase, everything of the phase before) is not written.
// Work per group: the cluster's vertices (<= 128), two rounds of 64 triangles, each at most SW_MAX_SPAN^2 pixel tests.
//   P[0] visible SRV (uint2), sorted lists SRV (raw), state UAV (raw), list
//   P[1] phase, list capacity, views SRV, bins SRV (raw: RasterBins.hlsl's header)
//   P[2] 64-bit target UAV (RWStructuredBuffer<uint64_t>, width x height), depth SRV, width, height
//   P[3] 1: a mirrored view (front faces are clockwise on screen), the frame's cluster page table's SRV + 1 (0: none), 0, 0
#include "Passes/Visibility/VisibilityCommon.hlsli"
#include "Passes/Visibility/RasterSw.hlsli"
#include "VisBuffer.hlsli"

#if MODE == 0
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint64_t> target = ResourceDescriptorHeap[P[2].x];
    const uint i = id.x + id.y * 65535u * 64u;
    if (i < P[2].z * P[2].w) target[i] = 0;
}
#else
groupshared float3 gs_pixel[128];  // x, y in target pixels, device depth

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer bins = ResourceDescriptorHeap[P[1].w];
    const uint list = P[0].w, capacity = P[1].y;
    // (RasterBins.hlsl: word 'list' the hardware entries of the phase, 4 + list the software ones, 8 + list the first)
    const uint first = bins.Load(4 * (8 + list)) + bins.Load(4 * list), count = bins.Load(4 * (4 + list));
    const uint index = group.x + group.y * 65535;
    if (index >= count || first + index >= capacity) return;  // uniform over the group
    const uint listEntry = lists.Load(4 * (list * capacity + first + index));
    if ((listEntry & LIST_ENTRY_SOFTWARE) == 0) return;  // (a defect: the run past the hardware entries is the software one)
    const uint visibleIndex = listEntry & ~LIST_ENTRY_FLAGS;
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = visible[visibleIndex];
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[itemView(entry)];
    const GpuInstance inst = loadInstance(itemInstance(entry));
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(itemIndex(entry));
    const uint vertexCount = min(clusterVertexCount(cl), 128u), triangleCount = min(clusterTriangleCount(cl), 128u);
    // (visibility.cluster_compression: the cluster's own vertices, in the stream or in its resident page)
    const ClusterVertexSource vertexSource = clusterVertexSource(itemIndex(entry), P[3].y);
    for (uint i = lane; i < vertexCount; i += 64)
    {
        VertexData vertex;
        const DeformedVertex d = deformClusterVertex(inst, mesh, cl, vertexSource, i, vertex);
        const float4 p = mul(v.viewProj, float4(d.world, 1));
        const float iw = 1.0 / p.w;  // (the classification: the cluster's sphere lies in front of the near plane)
        gs_pixel[i] = float3((p.x * iw * 0.5 + 0.5) * v.viewportSize.x, (0.5 - p.y * iw * 0.5) * v.viewportSize.y, p.z * iw);
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    RWStructuredBuffer<uint64_t> target = ResourceDescriptorHeap[P[2].x];
    Texture2D<float> depthBuffer = ResourceDescriptorHeap[P[2].y];
    const int2 size = int2(P[2].z, P[2].w);
    // Back faces: a one-sided list keeps the front faces - counter-clockwise on screen, clockwise in a mirrored view.
    const int facing = list == LIST_A_BACK ? (P[3].x != 0 ? -1 : 1) : 0;
    uint drawn = 0;
    bool cut = false;
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        const SwTriangle tri = swSetup(gs_pixel[packed & 0xFFu], gs_pixel[(packed >> 8) & 0xFFu], gs_pixel[(packed >> 16) & 0xFFu], facing, int2(0, 0), size - 1);
        cut = cut || tri.cut;
        if (!tri.valid) continue;
        ++drawn;
        const uint visId = packVisId(visibleIndex, t);
        int3 row = tri.edge;
        for (int y = tri.lo.y; y <= tri.hi.y; ++y)
        {
            int3 e = row;
            for (int x = tri.lo.x; x <= tri.hi.x; ++x)
            {
                if (min(e.x, min(e.y, e.z)) >= 0)
                {
                    const float depth = swDepth(tri, e);
                    // (reversed Z: a pixel the depth buffer already holds nearer keeps it; depth 0 is "nothing")
                    if (depth > 0 && depth >= depthBuffer.Load(int3(x, y, 0)))
                        InterlockedMax(target[(uint)y * (uint)size.x + (uint)x], ((uint64_t)asuint(min(depth, 1.0)) << 32) | (uint64_t)visId);
                }
                e += tri.stepX;
            }
            row += tri.stepY;
        }
    }
    // The group's statistics (every lane is back here), and a rectangle past the loop bound (a defect of the bound).
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
