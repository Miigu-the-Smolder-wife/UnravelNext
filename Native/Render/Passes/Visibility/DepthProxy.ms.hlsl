// unx-kernel: ms_6_6 main
// unx-variants: TILE=0,2
// Depth raster service, small casters as proxies (DepthRasterRequest::proxies; S's coarse shadow levels). A view with a
// smallest instance (RasterView::minInstanceTexels) leaves out the instances whose bounds project under it; with proxies
// each of them that is a member of an instance chunk is drawn instead as one square facing the view at its bounds'
// centre, of the area its silhouette fills of its bounding disc (P[1].x x pi r^2). A square under a texel covers a texel
// centre as often as its area is of a texel, so many of them darken a coarse level by the share of the ground they
// cover - what a forest's trees or a meadow's blades do to a level whose texel is larger than they are - at two
// triangles each, with no hierarchy traversal and no cluster.
// One group per PROXY_MEMBERS members of a visible chunk item of the run (CullChunks PHASE=1: chunk work, VS_CHUNK_ITEMS;
// CHUNK_INSTANCES / PROXY_MEMBERS groups per item, CullPrepare MODE=3: VA_PROXIES). A member is drawn when the run's
// instance tests keep it (mask, hidden, batch, set, frustum) and instanceBelowView leaves it out of the cull - the test
// that keeps it from CullInstances, so every member is drawn once: by its clusters or by its proxy. Instances of the
// flat list (dynamic, skinned, run-time) have no proxy.
// TILE=2 (the tile atlas): the square goes to the atlas slot of the tile its centre is in, clipped to that tile (what
// of it crosses into a neighbouring tile - under a texel wide - is left out); a tile that is not set draws nothing.
// TILE=0: the view's viewport.
//   P[0] chunk work SRV (uint2: chunk, view | CHUNK_ITEM_PROXIES), instance mask, state SRV (raw), deferred item capacity
//   P[1] asuint(the share of its bounding disc a caster fills), 0, views SRV, viewport per view (0 = one viewport)
//   P[2] 0, atlas slots SRV (raw, TILE=2), atlas tiles per row, atlas size (w | h << 16)
//   P[3] tile mask SRV (raw; UNX_NONE: none)
#include "Passes/Visibility/VisibilityCommon.hlsli"

#define PROXY_MEMBERS 64u
#define PROXY_GROUPS (CHUNK_INSTANCES / PROXY_MEMBERS)  // groups per chunk item

struct VertexOut
{
    float4 position : SV_Position;
#if TILE
    float4 clip : SV_ClipDistance0;  // >= 0 inside the centre's tile (left, right, top, bottom)
#endif
};

struct PrimitiveOut
{
#if TILE != 2
    uint viewport : SV_ViewportArrayIndex;
#endif
    bool cull : SV_CullPrimitive;
};

[outputtopology("triangle")]
[numthreads(PROXY_MEMBERS, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[4 * PROXY_MEMBERS], out primitives PrimitiveOut prims[2 * PROXY_MEMBERS],
          out indices uint3 tris[2 * PROXY_MEMBERS])
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    const uint g = group.x + group.y * 65535;
    const uint item = g / PROXY_GROUPS, part = g % PROXY_GROUPS;
    const bool valid = item < min(state.Load(4 * VS_CHUNK_ITEMS), P[0].w);  // uniform over the group
    StructuredBuffer<uint2> work = ResourceDescriptorHeap[P[0].x];
    const uint2 chunkItem = valid ? work[item] : uint2(0, 0);
    const uint view = chunkItem.y & ~CHUNK_ITEM_PROXIES;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[view];
    const CullScene cs = loadCullScene(v.cullSceneSrv);
    StructuredBuffer<CullChunk> chunks = ResourceDescriptorHeap[cs.chunkSrv];
    const CullChunk ch = chunks[chunkItem.x];
    const uint members = valid && (v.flags & CULL_VIEW_PROXIES) != 0 ? min(ch.count, CHUNK_INSTANCES) : 0;
    const uint first = part * PROXY_MEMBERS;
    const uint count = first < members ? min(members - first, PROXY_MEMBERS) : 0;  // uniform over the group
    SetMeshOutputCounts(4 * count, 2 * count);
    if (lane >= count) return;
    StructuredBuffer<uint> chunkInstances = ResourceDescriptorHeap[cs.chunkInstancesSrv];
    const uint instance = chunkInstances[ch.first + first + lane];
    const GpuInstance inst = loadInstance(instance);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const bool inBatch = v.instanceEnd == 0 || (instance >= v.instanceFirst && instance < v.instanceEnd);
    const float4 bounds = worldSphere(inst, inst.objectToWorld, mesh.boundsSphere);
    bool draw = instanceInRun(inst, P[0].y) && inBatch && instanceInSet(v, inst, instance) && frustumVisible(v, bounds) && instanceBelowView(v, bounds);
    const float4 centre = mul(v.viewProj, float4(bounds.xyz, 1));
    draw = draw && centre.w > 0;
#if TILE
    // the tile of the square's centre: set, and its atlas slot
    const float2 centrePx = centre.w > 0 ? float2((centre.x / centre.w * 0.5 + 0.5) * v.viewportSize.x, (0.5 - centre.y / centre.w * 0.5) * v.viewportSize.y) : float2(-1, -1);
    draw = draw && all(centrePx >= 0) && all(centrePx < v.viewportSize);
    const uint2 tile = uint2(clamp(centrePx, 0, v.viewportSize - 1)) / v.tilePx;
    const uint tileIndex = tile.y * v.tilesX + tile.x;
    if (P[3].x != UNX_NONE && v.cullMaskOffset != UNX_NONE)
    {
        ByteAddressBuffer mask = ResourceDescriptorHeap[P[3].x];
        draw = draw && ((mask.Load(4 * (v.cullMaskOffset + (tileIndex >> 5))) >> (tileIndex & 31u)) & 1u) != 0;
    }
    const float2 lo = float2(tile) * v.tilePx, hi = min(lo + v.tilePx, v.viewportSize);
    const float2 ndcLo = float2(2 * lo.x / v.viewportSize.x - 1, 1 - 2 * lo.y / v.viewportSize.y);
    const float2 ndcHi = float2(2 * hi.x / v.viewportSize.x - 1, 1 - 2 * hi.y / v.viewportSize.y);
    // tile -> slot by a whole-pixel shift in clip space (DepthRaster.ms.hlsl TILE=2)
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[2].y];
    const uint slot = draw ? slots.Load(4 * (v.cullMaskOffset * 32 + tileIndex)) : 0;
    const float2 atlasSize = float2(P[2].w & 0xFFFFu, P[2].w >> 16);
    const float2 shift = float2(slot % P[2].z, slot / P[2].z) * v.tilePx - lo;
    const float2 scale = v.viewportSize / atlasSize;
    const float2 offset = float2(scale.x - 1 + 2 * shift.x / atlasSize.x, 1 - scale.y - 2 * shift.y / atlasSize.y);
#endif
    // the square: the view's right and up (rows 0 and 1 of an orthographic or a face projection), half its side each way
    const float halfSide = 0.5 * bounds.w * sqrt(3.14159265 * asfloat(P[1].x));
    const float3 right = normalize(v.viewProj[0].xyz) * halfSide, up = normalize(v.viewProj[1].xyz) * halfSide;
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        const float3 world = bounds.xyz + ((c & 1) != 0 ? right : -right) + ((c & 2) != 0 ? up : -up);
        const float4 p = mul(v.viewProj, float4(world, 1));
        VertexOut o;
#if TILE
        o.position = float4(p.x * scale.x + p.w * offset.x, p.y * scale.y + p.w * offset.y, p.z, p.w);
        o.clip = float4(p.x - ndcLo.x * p.w, ndcHi.x * p.w - p.x, ndcLo.y * p.w - p.y, p.y - ndcHi.y * p.w);
#else
        o.position = p;
#endif
        verts[4 * lane + c] = o;
    }
    tris[2 * lane] = uint3(4 * lane, 4 * lane + 1, 4 * lane + 2);
    tris[2 * lane + 1] = uint3(4 * lane + 1, 4 * lane + 3, 4 * lane + 2);
    [unroll] for (uint t = 0; t < 2; ++t)
    {
#if TILE != 2
        prims[2 * lane + t].viewport = P[1].w != 0 ? view : 0;
#endif
        prims[2 * lane + t].cull = !draw;
    }
}
