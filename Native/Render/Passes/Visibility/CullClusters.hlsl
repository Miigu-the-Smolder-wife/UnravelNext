// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Cluster culling and band classification.
//   MODE=0: one 32-thread group per group item (a hierarchy leaf reached by the traversal) in [VS_GROUP_BEGIN,
//           VS_GROUP_END); each thread tests one cluster of the group (own-error LOD test: the group's parent test
//           was the leaf's).
//   MODE=1: one thread per cluster deferred by phase 1 (phase 2 only). The phase is the root constant (CULL_PHASE).
// Tests: own error <= threshold, frustum + clip plane, normal cone (one-sided, undeformed), HiZ occlusion (phase 1:
// previous frame, occluded clusters deferred; phase 2: this frame), tile mask (raster service). Visible clusters get a
// visible-list entry (the vis id's cluster index) and an entry in their band / pipeline list; in a tile-local raster
// run (TILE_PAIRS_UAV) one entry per (cluster, tile rectangle) pair instead (tileVisit).
// Bands (ARCHITECTURE 2.1): w_face = projected minimum feature width seen face-on, w_min = w_face x (flat sheets) the
// smallest |cos| between the view direction and the cluster's normals. A: w_min >= band A minimum; C: w_face < band C
// maximum; B otherwise. The run's band mode picks the list of each band (BAND_MODE_*, CullShared.hlsli).
#include "Passes/Visibility/CullShared.hlsli"

#define PI 3.14159265

struct ClusterResult
{
    bool visible, defer, wholeRange, mixed;
    uint list, list2, band, triangles, pairs;  // list2: the coverage list of a mixed sheet cluster (else VS_LISTS)
    uint2 tileA, tileB;
};

ClusterResult testCluster(uint instance, uint clusterIndex, uint view)
{
    ClusterResult r = (ClusterResult)0;
    const GpuInstance inst = loadInstance(instance);
    const GpuCluster cl = loadCluster(clusterIndex);
    const CullView v = loadView(view);
    const float scale = instanceScale(inst);
    const bool skinned = (inst.flags & INSTANCE_SKINNED) != 0, wind = (inst.flags & INSTANCE_WIND) != 0;
    // C4: blend shapes / vertex animation change positions (inside the inflated spheres) and normals: bind-pose normal
    // cones and sheet orientations do not bound them, as for skinned instances.
    const bool deformed = skinned || inst.morph != UNX_NONE;
    // Own error: drawn only where its own simplification is fine enough (source clusters: error 0).
    if (cl.lodError > 0)
    {
        StructuredBuffer<float4> spheres = ResourceDescriptorHeap[LOD_SPHERES_SRV];
        const float4 lodSphere = spheres[clusterIndex];
        if (patchForcesSource(inst, lodSphere)) return r;  // C5: the source clusters are drawn there instead
        if (projectedError(v, worldSphere(inst, inst.objectToWorld, lodSphere), cl.lodError * scale) > v.lodThreshold) return r;
    }
    const float4 s = worldSphere(inst, inst.objectToWorld, cl.boundsSphere);
    const bool unbounded = skinned || (inst.flags & INSTANCE_VIEW_MODEL) != 0;  // A12 view models: remapped projection
    if (!unbounded && !frustumVisible(v, s)) return r;
    const GpuMaterial m = loadMaterial(clusterMaterial(inst, cl));
    const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0, alpha = (m.classFlags & MATERIAL_ALPHA_TESTED) != 0;
    const bool cullBack = (v.flags & CULL_VIEW_CULL_BACK) != 0 && !twoSided;
    const float3 axis = normalize(transformVector(inst.objectToWorld, cl.normalCone.xyz));
    const float3 toCluster = s.xyz - v.position;
    const float dist = length(toCluster);
    if (cullBack && !deformed && !wind && cl.normalCone.w < 1)
    {
        const bool back = v.orthographic ? dot(v.viewDirection.xyz, axis) >= cl.normalCone.w : dot(toCluster, axis) >= cl.normalCone.w * dist + s.w;
        if (back) return r;
    }
    if (!unbounded && (v.flags & CULL_VIEW_OCCLUSION) != 0)
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
    if (TILE_PAIRS_UAV != UNX_NONE)
    {
        if (!tileRange(v, s, r.tileA, r.tileB)) return r;
        r.pairs = tileVisit(TILE_VISIT_COUNT, v, view, tileMasks(), r.tileA, r.tileB, r.wholeRange, 0, 0, 0, 0, 0, UNX_NONE, UNX_NONE);
        if (r.pairs == 0) return r;
    }
    else if (!tileVisible(v, view, s))
        return r;

    // Band.
    const float w = cl.minFeatureWidth;
    const float wFace = projectedLength(v, s, abs(w) * scale);
    float wMin = wFace;
    if (w < 0 && !deformed)
    {
        // Flat sheet of width r: its projected width shrinks with the view angle. The builder's sheet orientation
        // (clusterSheets: winding-independent axis, spread, slab) bounds it:
        //   orthographic, or perspective with r <= 2R (the cluster spans the sheet's width: cards, blades): the
        //     smallest |cos| between a view ray and a sheet normal, cos(alpha + spread), where alpha is the largest
        //     angle between a ray and the axis (orthographic: the view direction; perspective: cos alpha >=
        //     (eye's distance to the slab) / (d + R));
        //   perspective with r > 2R (wide sheets: terrain): the width chord foreshortened by (eye's distance to the
        //     slab) / (d + R + r); far wide sheets at grazing become thin near the horizon, as they are on screen.
        // Spread >= 90 degrees (no sheet axis): 0. A planar sheet with the eye in its plane: 0.
        StructuredBuffer<float4> sheets = ResourceDescriptorHeap[SHEETS_SRV];
        const float4 sheet = sheets[clusterIndex];
        const float cosSpread = length(sheet.xyz);
        float factor = 0;
        if (cosSpread > 1e-4)
        {
            const float3 sheetAxis = normalize(transformVector(inst.objectToWorld, sheet.xyz));
            const float spread = acos(saturate(cosSpread)), r = abs(w) * scale;
            float cosAlpha;
            if (v.orthographic) cosAlpha = abs(dot(v.viewDirection.xyz, sheetAxis));
            else
            {
                const float h = max(abs(dot(sheetAxis, toCluster)) - sheet.w * scale, 0.0);
                cosAlpha = r <= 2 * s.w ? h / (dist + s.w) : h / (dist + s.w + r);
            }
            if (!v.orthographic && r > 2 * s.w) factor = cosAlpha;
            else
            {
                const float total = acos(saturate(cosAlpha)) + spread;
                factor = total >= 0.5 * PI ? 0.0 : cos(total);
            }
        }
        wMin = wFace * factor;
    }
    else if (w < 0)
        wMin = 0;  // skinned sheets: bind-pose normals do not bound the posed ones
    // Sheet clusters that may hold band B triangles are split per triangle by the mesh kernels (the band is defined
    // per element; the cluster bound above only says which clusters can be mixed). Others keep the cluster rule, with
    // hysteresis (a): band B until the width reaches the hysteresis width if it was band B from the previous camera
    // (solid clusters: the width scales with 1 / distance).
    const bool sheet = w < 0 && !deformed;
    r.mixed = BAND_MODE != BAND_MODE_A && sheet && wFace >= BAND_C_MAX_PX && wMin < v.bandAHysteresisPx;
    bool bandB = wMin < BAND_A_MIN_PX;
    if (!bandB && !sheet && wMin < v.bandAHysteresisPx && !v.orthographic)
    {
        const float dNow = max(length(s.xyz - v.position) - s.w, v.nearPlane), dPrev = max(length(s.xyz - v.prevPosition) - s.w, v.nearPlane);
        bandB = wMin * dNow / dPrev < BAND_A_MIN_PX;
    }
    r.band = r.mixed ? 1u : (!bandB ? 0u : (wFace < BAND_C_MAX_PX ? 2u : 1u));
    const uint drawBand = r.mixed ? 0u : (BAND_MODE == BAND_MODE_A ? 0u : (BAND_MODE == BAND_MODE_COVERAGE ? min(r.band, 1u) : r.band));
    r.list = drawBand == 1 ? LIST_B : (drawBand == 2 ? LIST_C : (alpha ? (cullBack ? LIST_A_ALPHA_BACK : LIST_A_ALPHA_NONE) : (cullBack ? LIST_A_BACK : LIST_A_NONE)));
    r.list2 = r.mixed ? LIST_B : VS_LISTS;
    r.triangles = clusterTriangleCount(cl);
    r.visible = true;
    return r;
}

void emit(RWByteAddressBuffer state, bool active, uint instance, uint clusterIndex, uint view, ClusterResult r)
{
    const bool visible = active && r.visible, defer = active && r.defer;
    const bool tileLocal = TILE_PAIRS_UAV != UNX_NONE;
    const uint entries = visible ? (tileLocal ? r.pairs : 1) : 0;
    const uint idx = waveAppend(state, VS_VISIBLE, visible ? 1 : 0, CAP_VISIBLE, OVERFLOW_VISIBLE);
    uint slot[VS_LISTS];
    [unroll] for (uint k = 0; k < VS_LISTS; ++k)
        slot[k] = waveAppend(state, VS_LIST_COUNT + k, (r.list == k || r.list2 == k) ? entries : 0, CAP_VISIBLE, OVERFLOW_VISIBLE);
    const uint pairBase = waveAppend(state, VS_TILE_PAIRS, tileLocal ? entries : 0, CAP_VISIBLE, OVERFLOW_TILE_PAIRS);
    if (visible && idx < CAP_VISIBLE)
    {
        RWStructuredBuffer<uint2> visibleList = ResourceDescriptorHeap[VISIBLE_UAV];
        visibleList[idx] = uint2(instance, packItem(clusterIndex, view));
        RWByteAddressBuffer lists = ResourceDescriptorHeap[LISTS_UAV];
        uint s = 0, s2 = 0;
        [unroll] for (uint k = 0; k < VS_LISTS; ++k)
        {
            if (r.list == k) s = slot[k];
            if (r.list2 == k) s2 = slot[k];
        }
        if (tileLocal)
            tileVisit(TILE_VISIT_WRITE, loadView(view), view, tileMasks(), r.tileA, r.tileB, r.wholeRange, idx, pairBase, s, r.list, CAP_VISIBLE, TILE_PAIRS_UAV,
                      LISTS_UAV);
        else
        {
            const uint e = idx | (r.mixed ? LIST_ENTRY_MIXED : 0u);
            if (s < CAP_VISIBLE) lists.Store(4 * (r.list * CAP_VISIBLE + s), e);
            if (r.mixed && s2 < CAP_VISIBLE) lists.Store(4 * (r.list2 * CAP_VISIBLE + s2), e);
        }
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
        const uint n = WaveActiveCountBits(visible && r.band == b);
        if (WaveIsFirstLane() && n > 0) state.InterlockedAdd(4 * (VS_STAT_BAND_CLUSTERS + b), n);
    }
    const uint mixedClusters = WaveActiveCountBits(visible && r.mixed), mixedTriangles = WaveActiveSum((visible && r.mixed) ? r.triangles : 0);
    if (WaveIsFirstLane() && mixedClusters > 0)
    {
        state.InterlockedAdd(4 * VS_STAT_MIXED_CLUSTERS, mixedClusters);
        state.InterlockedAdd(4 * VS_STAT_MIXED_TRIANGLES, mixedTriangles);
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
