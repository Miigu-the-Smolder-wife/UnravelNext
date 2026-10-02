// unx-kernel: cs_6_6 main
// unx-variants: PHASE=1,2
// Instance chunk culling (C3, VisibilityCommon.hlsli CullScene), ahead of the instance pass. PHASE=1: every chunk x every
// view (dispatch y = view): frustum + clip plane, raster-service tile mask, and for occlusion views the previous frame's
// HiZ (members are static: previous bounds = current): visible chunks become items of the chunk instance pass
// (CullInstances SOURCE=1), occluded ones are deferred. PHASE=2: the deferred chunks against this frame's HiZ; the
// members of those that pass are appended to the deferred instances, which CullInstances PHASE=2 tests one by one
// against the same HiZ. A chunk's sphere is inflated by its wind term x this frame's wind speed^2 (ChunkBounds.hlsl), so
// it bounds every member's worldSphere. Chunk items and deferred chunks live in the chunk work buffer
// (CHUNK_WORK_UAV: [0, CAP_DEFERRED) items, [CAP_DEFERRED, 2 CAP_DEFERRED) deferred chunks).
#include "Passes/Visibility/CullShared.hlsli"

float4 chunkSphere(CullChunk ch)
{
    return float4(ch.sphere.xyz, ch.sphere.w + asfloat(ch.pad0) * g_windSpeed * g_windSpeed);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWStructuredBuffer<uint2> work = ResourceDescriptorHeap[CHUNK_WORK_UAV];
    uint chunk = 0, view = 0;
    bool valid;
#if PHASE == 1
    chunk = id.x;
    view = id.y;
    const CullView cv = loadView(min(view, VIEW_COUNT - 1));
    const CullScene cs = loadCullScene(cv.cullSceneSrv);
    valid = chunk < cs.chunkCount && view < VIEW_COUNT && !cullViewTilesEmpty(cv, view);
#else
    const uint count = min(state.Load(4 * VS_DEFER_CHUNKS), CAP_DEFERRED);
    valid = id.x < count;
    if (valid)
    {
        const uint2 item = work[CAP_DEFERRED + id.x];
        chunk = item.x;
        view = item.y;
    }
    const CullScene cs = loadCullScene(loadView(view).cullSceneSrv);
#endif
    bool visible = false, defer = false, proxies = false;
    CullChunk ch = (CullChunk)0;
    if (valid)
    {
        StructuredBuffer<CullChunk> chunks = ResourceDescriptorHeap[cs.chunkSrv];
        ch = chunks[chunk];
        const CullView v = loadView(view);
        const bool bounded = ch.sphere.w >= 0;  // ChunkBounds ran (always before the first cull of a revision)
        const float4 s = chunkSphere(ch);
        visible = ch.count > 0 && (!bounded || (frustumVisible(v, s) && tileVisible(v, view, s)));
        if (visible && bounded && (v.flags & CULL_VIEW_OCCLUSION) != 0)
        {
#if PHASE == 1
            if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, s))
            {
                visible = false;
                defer = true;
            }
#else
            visible = !hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, s);
#endif
        }
        // (a view that draws proxies: a chunk whose every member is under its smallest instance is not expanded)
        proxies = visible && bounded && chunkBelowView(v, ch, s);
    }
#if PHASE == 1
    const uint a = waveAppend(state, VS_CHUNK_ITEMS, visible ? 1 : 0, CAP_DEFERRED, OVERFLOW_CHUNK_ITEMS);
    if (visible && a < CAP_DEFERRED) work[a] = uint2(chunk, view | (proxies ? CHUNK_ITEM_PROXIES : 0u));
    // the chunk instance pass's arguments (one group per item; CullPrepare MODE=4 stores the same)
    const uint added = WaveActiveCountBits(visible);
    if (added > 0 && WaveIsFirstLane())  // (the first lane's 'a' is the wave's first item)
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[ARGS_UAV];
        raiseDispatch(args, VA_CHUNK_ITEMS, min(a + added, CAP_DEFERRED));
    }
    const uint d = waveAppend(state, VS_DEFER_CHUNKS, defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_CHUNKS);
    if (defer && d < CAP_DEFERRED) work[CAP_DEFERRED + d] = uint2(chunk, view);
#else
    const uint n = visible ? min(ch.count, CHUNK_INSTANCES) : 0;
    const uint base = waveAppend(state, VS_DEFER_INSTANCES, n, CAP_DEFERRED, OVERFLOW_DEFER_INSTANCES);
    if (n > 0)
    {
        StructuredBuffer<uint> members = ResourceDescriptorHeap[cs.chunkInstancesSrv];
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_INSTANCES_UAV];
        [loop] for (uint k = 0; k < n; ++k)  // n <= CHUNK_INSTANCES
            if (base + k < CAP_DEFERRED) deferred[base + k] = uint2(members[ch.first + k], view);
    }
    const uint s2 = WaveActiveCountBits(visible);
    if (WaveIsFirstLane() && s2 > 0) state.InterlockedAdd(4 * VS_STAT_CHUNKS, s2);
#endif
}
