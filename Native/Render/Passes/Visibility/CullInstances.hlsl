// unx-kernel: cs_6_6 main
// unx-variants: PHASE=1,2 SOURCE=0,1,2
// Instance culling. PHASE=1 SOURCE=0: the run's flat instances (CullScene: dynamic and skinned instances, and every
// instance outside a chunk) x every view (dispatch y = view); PHASE=1 SOURCE=1: one group per visible chunk item
// (CullChunks PHASE=1), its members (<= CHUNK_INSTANCES: four passes of 64). Frustum + clip plane, raster-service tile
// mask, the view's smallest instance (shadow views: instanceBelowView), and for views with occlusion the previous frame's
// HiZ with previous transforms: occluded instances are deferred
// to phase 2, visible ones push their per-depth hierarchy roots as node items. PHASE=2 (SOURCE unused): the deferred
// instances, and the members of chunks that passed CullChunks PHASE=2, against this frame's HiZ.
// Skinned instances are tested with their palette bounds (SkinBounds.hlsl; the previous palette's in phase 1). A skinned
// instance whose mesh has no skin data is not deformed, so its rigid bounds hold; one without palette bounds (too many
// joints) stays visible, as its nodes and clusters do (their bind-pose bounds do not bound posed vertices).
#include "Passes/Visibility/CullShared.hlsli"

void cullInstance(RWByteAddressBuffer state, uint instance, uint view, bool valid)
{
    bool visible = false, defer = false;
    MeshClusterRoots roots = (MeshClusterRoots)0;
    if (valid)
    {
        const GpuInstance inst = loadInstance(instance);
        const GpuMesh mesh = loadMesh(inst.mesh);
        StructuredBuffer<MeshClusterRoots> rootBuffer = ResourceDescriptorHeap[ROOTS_SRV];
        roots = rootBuffer[inst.mesh];
        const CullView v = loadView(view);
        const bool inBatch = v.instanceEnd == 0 || (instance >= v.instanceFirst && instance < v.instanceEnd);  // (RasterView's instance batch)
        if (instanceInRun(inst, INSTANCE_MASK) && inBatch && instanceInSet(v, inst, instance))
        {
            float4 bounds = worldSphere(inst, inst.objectToWorld, mesh.boundsSphere);
            float4 prevBounds = worldSphere(inst, inst.prevObjectToWorld, mesh.boundsSphere);
            bool bounded = true;
            if ((inst.flags & INSTANCE_SKINNED) != 0 && inst.bonePalette != UNX_NONE && mesh.skinOffset != UNX_NONE)
            {
                bounded = false;
                const CullScene cs = loadCullScene(v.cullSceneSrv);
                const uint slot = skinSlot(cs, instance);
                if (slot != UNX_NONE)
                {
                    StructuredBuffer<float4> skinBounds = ResourceDescriptorHeap[cs.skinBoundsSrv];
                    bounds = skinBounds[2 * slot];
                    prevBounds = skinBounds[2 * slot + 1];
                    bounded = bounds.w >= 0 && prevBounds.w >= 0;
                }
            }
            // A12 view models: the main view draws them with its projection remapped (ViewModel.hlsli viewModelClip), so the
            // view's planes and HiZ do not bound them; a few clusters, drawn untested.
            if ((inst.flags & INSTANCE_VIEW_MODEL) != 0) bounded = false;
            const bool twoPhase = (v.flags & CULL_VIEW_TILE_TWO_PHASE) != 0;
            visible = roots.rootCount > 0 && (!bounded || (frustumVisible(v, bounds) && !instanceBelowView(v, bounds) && tileVisible(v, view, bounds) &&
                                                           (twoPhase || !tilesOcclude(v, TILE_MASK_SRV, bounds, false))));
            if (visible && bounded && twoPhase)
            {
                // tile occluders in two phases (VisibilityCommon.hlsli tilesOcclude): as the HiZ's two phases below
#if PHASE == 1
                if (tilesOcclude(v, TILE_MASK_SRV, bounds, true))
                {
                    visible = false;
                    defer = true;
                }
#else
                visible = !tilesOcclude(v, TILE_MASK_SRV, bounds, false);
#endif
            }
            if (visible && bounded && (v.flags & CULL_VIEW_OCCLUSION) != 0)
            {
#if PHASE == 1
                if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, prevBounds))
                {
                    visible = false;
                    defer = true;
                }
#else
                visible = !hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, bounds);
#endif
            }
        }
    }
    uint first, total;
    const uint base = nodeReserve(state, visible ? roots.rootCount : 0, CAP_NODES, 0, first, total);
    if (visible)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        for (uint k = 0; k < roots.rootCount; ++k)
            if (base + k < CAP_NODES) items[base + k] = packItem(instance, roots.nodeOffset + k, view);
    }
    nodePublish(state, first, total);
    nodePublishRange(base, visible ? roots.rootCount : 0u, true);
    const uint d = waveAppend(state, VS_DEFER_INSTANCES, defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_INSTANCES);
    if (defer && d < CAP_DEFERRED)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_INSTANCES_UAV];
        deferred[d] = uint2(instance, view);
    }
    const uint s = WaveActiveCountBits(visible);
    if (WaveIsFirstLane() && s > 0) state.InterlockedAdd(4 * VS_STAT_INSTANCES, s);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
#if PHASE == 1 && SOURCE == 0
    const CullView v0 = loadView(min(id.y, VIEW_COUNT - 1));
    const CullScene cs = loadCullScene(v0.cullSceneSrv);
    // The flat list, then the C2b runtime instances (added between frames, no scene revision). The GPU-written ones:
    // SOURCE=2 (indirect, from their live count).
    const uint cpuCount = cs.flatCount + v0.runtimeCount;
    const bool valid = id.y < VIEW_COUNT && id.x < cpuCount && !cullViewTilesEmpty(v0, id.y);
    uint instance = 0;
    if (valid && id.x < cs.flatCount)
    {
        StructuredBuffer<uint> flat = ResourceDescriptorHeap[cs.flatSrv];
        instance = flat[id.x];
    }
    else if (valid)
        instance = v0.runtimeFirst + (id.x - cs.flatCount);
    cullInstance(state, instance, id.y, valid);
#elif PHASE == 1 && SOURCE == 2
    // The GPU-written instances (A3 mesh particles): ceil(live / 64) x views groups (CullReset, VA_GPU_INSTANCES).
    const CullView v0 = loadView(min(id.y, VIEW_COUNT - 1));
    const bool valid = id.y < VIEW_COUNT && id.x < gpuInstanceCount(v0) && !cullViewTilesEmpty(v0, id.y);
    cullInstance(state, valid ? v0.gpuFirst + id.x : 0, id.y, valid);
#elif PHASE == 1
    // One group per chunk item: the item and the chunk are uniform over the group. An item whose members are all drawn
    // as proxies (CHUNK_ITEM_PROXIES) has none to test.
    const uint item = gid.x + gid.y * 65535u;
    const bool any = item < min(state.Load(4 * VS_CHUNK_ITEMS), CAP_DEFERRED);
    uint view = 0, first = 0, count = 0, membersSrv = 0;
    if (any)
    {
        RWStructuredBuffer<uint2> work = ResourceDescriptorHeap[CHUNK_WORK_UAV];
        const uint2 chunkItem = work[item];
        view = chunkItem.y & ~CHUNK_ITEM_PROXIES;
        const CullScene cs = loadCullScene(loadView(view).cullSceneSrv);
        StructuredBuffer<CullChunk> chunks = ResourceDescriptorHeap[cs.chunkSrv];
        const CullChunk ch = chunks[chunkItem.x];
        first = ch.first;
        count = (chunkItem.y & CHUNK_ITEM_PROXIES) != 0 ? 0u : min(ch.count, CHUNK_INSTANCES);
        membersSrv = cs.chunkInstancesSrv;
    }
    [unroll] for (uint base = 0; base < CHUNK_INSTANCES; base += 64)
    {
        const bool valid = base + lane < count;
        uint instance = 0;
        if (valid)
        {
            StructuredBuffer<uint> members = ResourceDescriptorHeap[membersSrv];
            instance = members[first + base + lane];
        }
        cullInstance(state, instance, view, valid);
    }
#else
    const uint count = min(state.Load(4 * VS_DEFER_INSTANCES), CAP_DEFERRED);
    const bool valid = id.x < count;
    uint instance = 0, view = 0;
    if (valid)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_INSTANCES_UAV];
        const uint2 item = deferred[id.x];
        instance = item.x;
        view = item.y;
    }
    cullInstance(state, instance, view, valid);
#endif
}
