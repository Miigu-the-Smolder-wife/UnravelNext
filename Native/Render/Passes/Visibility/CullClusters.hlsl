// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Cluster culling and band classification.
//   MODE=0: one 32-thread group per group item (a hierarchy leaf reached by the traversal) in [VS_GROUP_BEGIN,
//           VS_GROUP_END); each thread tests one cluster of the group (own-error LOD test: the group's parent test
//           was the leaf's).
//   MODE=1: one thread per cluster deferred by phase 1 (phase 2 only). The phase is the root constant (CULL_PHASE).
// Tests: own error <= threshold, frustum + clip plane, normal cone (one-sided, undeformed), HiZ occlusion (phase 1:
// previous frame, occluded clusters deferred; phase 2: this frame), tile mask (raster service). Visible clusters get a
// visible-list entry (the vis id's cluster index) and an entry in their band / pipeline list.
// Bands (ARCHITECTURE 2.1): w_face = projected minimum feature width seen face-on, w_min = w_face x (flat sheets) the
// smallest |cos| between the view direction and the cluster's normals. A: w_min >= band A minimum; C: w_face < band C
// maximum; B otherwise. BAND_MODE 0 puts every cluster in band A (until the coverage layer draws bands B and C).
#include "Passes/Visibility/CullShared.hlsli"

#define PI 3.14159265

struct ClusterResult
{
    bool visible, defer;
    uint list, band, triangles;
};

ClusterResult testCluster(uint instance, uint clusterIndex, uint view)
{
    ClusterResult r = (ClusterResult)0;
    const GpuInstance inst = loadInstance(instance);
    const GpuCluster cl = loadCluster(clusterIndex);
    const CullView v = loadView(view);
    const float scale = instanceScale(inst);
    const bool skinned = (inst.flags & INSTANCE_SKINNED) != 0, wind = (inst.flags & INSTANCE_WIND) != 0;
    // Own error: drawn only where its own simplification is fine enough (source clusters: error 0).
    if (cl.lodError > 0)
    {
        StructuredBuffer<float4> spheres = ResourceDescriptorHeap[LOD_SPHERES_SRV];
        if (projectedError(v, worldSphere(inst, inst.objectToWorld, spheres[clusterIndex]), cl.lodError * scale) > v.lodThreshold) return r;
    }
    const float4 s = worldSphere(inst, inst.objectToWorld, cl.boundsSphere);
    if (!skinned && !frustumVisible(v, s)) return r;
    const GpuMaterial m = loadMaterial(clusterMaterial(inst, cl));
    const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0, alpha = (m.classFlags & MATERIAL_ALPHA_TESTED) != 0;
    const bool cullBack = (v.flags & CULL_VIEW_CULL_BACK) != 0 && !twoSided;
    const float3 axis = normalize(transformVector(inst.objectToWorld, cl.normalCone.xyz));
    const float3 toCluster = s.xyz - v.position;
    const float dist = length(toCluster);
    if (cullBack && !skinned && !wind && cl.normalCone.w < 1)
    {
        const bool back = v.orthographic ? dot(v.viewDirection.xyz, axis) >= cl.normalCone.w : dot(toCluster, axis) >= cl.normalCone.w * dist + s.w;
        if (back) return r;
    }
    if (!skinned && (v.flags & CULL_VIEW_OCCLUSION) != 0)
    {
        if (CULL_PHASE == 1)
        {
            if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, worldSphere(inst, inst.prevObjectToWorld, cl.boundsSphere)))
            {
                r.defer = true;
                return r;
            }
        }
        else if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, s))
            return r;
    }
    if (TILE_MASK_SRV != UNX_NONE && !tileMaskCovered(v, TILE_MASK_SRV, s)) return r;

    // Band.
    const float w = cl.minFeatureWidth;
    const float wFace = projectedLength(v, s, abs(w) * scale);
    float wMin = wFace;
    if (w < 0)
    {
        float factor = 0;
        if (cl.normalCone.w < 1 && !skinned)
        {
            const float3 dir = v.orthographic ? v.viewDirection.xyz : toCluster / max(dist, 1e-6);
            const float spread = asin(saturate(cl.normalCone.w)) + (v.orthographic ? 0.0 : asin(saturate(s.w / max(dist, 1e-6))));
            const float total = acos(saturate(abs(dot(dir, axis)))) + spread;
            factor = total >= 0.5 * PI ? 0.0 : cos(total);
        }
        wMin = wFace * factor;
    }
    r.band = wMin >= BAND_A_MIN_PX ? 0u : (wFace < BAND_C_MAX_PX ? 2u : 1u);
    const uint drawBand = BAND_MODE == 0 ? 0u : r.band;
    r.list = drawBand == 1 ? LIST_B : (drawBand == 2 ? LIST_C : (alpha ? (cullBack ? LIST_A_ALPHA_BACK : LIST_A_ALPHA_NONE) : (cullBack ? LIST_A_BACK : LIST_A_NONE)));
    r.triangles = clusterTriangleCount(cl);
    r.visible = true;
    return r;
}

void emit(RWByteAddressBuffer state, bool active, uint instance, uint clusterIndex, uint view, ClusterResult r)
{
    const bool visible = active && r.visible, defer = active && r.defer;
    const uint idx = waveAppend(state, VS_VISIBLE, visible ? 1 : 0, CAP_VISIBLE, OVERFLOW_VISIBLE);
    uint slot[VS_LISTS];
    [unroll] for (uint k = 0; k < VS_LISTS; ++k)
        slot[k] = waveAppend(state, VS_LIST_COUNT + k, (visible && r.list == k) ? 1 : 0, CAP_VISIBLE, OVERFLOW_VISIBLE);
    if (visible && idx < CAP_VISIBLE)
    {
        RWStructuredBuffer<uint2> visibleList = ResourceDescriptorHeap[VISIBLE_UAV];
        visibleList[idx] = uint2(instance, packItem(clusterIndex, view));
        RWByteAddressBuffer lists = ResourceDescriptorHeap[LISTS_UAV];
        uint s = 0;
        [unroll] for (uint k = 0; k < VS_LISTS; ++k)
            if (r.list == k) s = slot[k];
        if (s < CAP_VISIBLE) lists.Store(4 * (r.list * CAP_VISIBLE + s), idx);
    }
    const uint d = waveAppend(state, VS_DEFER_CLUSTERS, defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_CLUSTERS);
    if (defer && d < CAP_DEFERRED)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_CLUSTERS_UAV];
        deferred[d] = uint2(instance, packItem(clusterIndex, view));
    }
    [unroll] for (uint b = 0; b < 3; ++b)
    {
        const uint t = WaveActiveSum((visible && r.band == b) ? r.triangles : 0);
        if (WaveIsFirstLane() && t > 0) state.InterlockedAdd(4 * (VS_STAT_TRIANGLES + b), t);
    }
    const uint tested = WaveActiveCountBits(active);
    if (WaveIsFirstLane() && tested > 0) state.InterlockedAdd(4 * VS_STAT_CLUSTERS, tested);
}

#if MODE == 0
[numthreads(32, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    const uint item = state.Load(4 * VS_GROUP_BEGIN) + group.x + group.y * 65535;  // 2D dispatch (65535 per row)
    if (item >= state.Load(4 * VS_GROUP_END)) return;  // uniform over the group
    RWStructuredBuffer<uint2> groups = ResourceDescriptorHeap[GROUP_ITEMS_UAV];
    const uint2 g = groups[item];
    StructuredBuffer<ClusterNode> nodes = ResourceDescriptorHeap[NODES_SRV];
    const ClusterNode leaf = nodes[itemIndex(g.y)];
    const uint view = itemView(g.y);
    for (uint base = 0; base < leaf.count; base += 32)  // uniform over the group: one leaf per group
    {
        const bool active = base + lane < leaf.count;
        ClusterResult r = (ClusterResult)0;
        if (active) r = testCluster(g.x, leaf.first + base + lane, view);
        emit(state, active, g.x, leaf.first + base + lane, view, r);
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    const bool active = i < min(state.Load(4 * VS_DEFER_CLUSTERS), CAP_DEFERRED);
    uint2 d = 0;
    ClusterResult r = (ClusterResult)0;
    if (active)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_CLUSTERS_UAV];
        d = deferred[i];
        r = testCluster(d.x, itemIndex(d.y), itemView(d.y));
        r.defer = false;
    }
    emit(state, active, d.x, itemIndex(d.y), itemView(d.y), r);
}
#endif
