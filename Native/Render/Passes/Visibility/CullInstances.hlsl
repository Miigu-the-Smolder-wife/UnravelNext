// unx-kernel: cs_6_6 main
// unx-variants: PHASE=1,2
// Instance culling. PHASE=1: every instance x every view (dispatch y = view); frustum + clip plane, raster-service tile
// mask, and for views with occlusion the previous frame's HiZ with previous transforms: occluded instances are deferred
// to phase 2, visible ones push their per-depth hierarchy roots as node items. PHASE=2: the deferred instances against
// this frame's HiZ.
#include "Passes/Visibility/CullShared.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    uint instance = 0, view = 0;
    bool valid;
#if PHASE == 1
    instance = id.x;
    view = id.y;
    valid = instance < INSTANCE_COUNT && view < VIEW_COUNT;
#else
    const uint count = min(state.Load(4 * VS_DEFER_INSTANCES), CAP_DEFERRED);
    valid = id.x < count;
    if (valid)
    {
        RWStructuredBuffer<uint> deferred = ResourceDescriptorHeap[DEFER_INSTANCES_UAV];
        const uint packed = deferred[id.x];
        instance = itemIndex(packed);
        view = itemView(packed);
    }
#endif
    bool visible = false, defer = false;
    MeshClusterRoots roots = (MeshClusterRoots)0;
    if (valid)
    {
        const GpuInstance inst = loadInstance(instance);
        const GpuMesh mesh = loadMesh(inst.mesh);
        StructuredBuffer<MeshClusterRoots> rootBuffer = ResourceDescriptorHeap[ROOTS_SRV];
        roots = rootBuffer[inst.mesh];
        const CullView v = loadView(view);
        const bool skinned = (inst.flags & INSTANCE_SKINNED) != 0;  // bind-pose bounds do not bound skinned vertices (P3)
        if (((inst.flags & INSTANCE_MASK) != 0 || INSTANCE_MASK == 0) && (inst.flags & INSTANCE_HIDDEN) == 0)
        {
            const float4 bounds = worldSphere(inst, inst.objectToWorld, mesh.boundsSphere);
            visible = roots.rootCount > 0 && (skinned || (frustumVisible(v, bounds) && tileVisible(v, view, bounds)));
            if (visible && !skinned && (v.flags & CULL_VIEW_OCCLUSION) != 0)
            {
#if PHASE == 1
                const bool occluded = hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, worldSphere(inst, inst.prevObjectToWorld, mesh.boundsSphere));
                if (occluded)
                {
                    visible = false;
                    defer = true;
                }
#else
                visible = !hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, worldSphere(inst, inst.objectToWorld, mesh.boundsSphere));
#endif
            }
        }
    }
    const uint base = waveAppend(state, VS_NODE_WRITE, visible ? roots.rootCount : 0, CAP_NODES, OVERFLOW_NODES);
    if (visible)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        for (uint k = 0; k < roots.rootCount; ++k)
            if (base + k < CAP_NODES) items[base + k] = uint2(instance, packItem(roots.nodeOffset + k, view));
    }
    const uint d = waveAppend(state, VS_DEFER_INSTANCES, defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_INSTANCES);
    if (defer && d < CAP_DEFERRED)
    {
        RWStructuredBuffer<uint> deferred = ResourceDescriptorHeap[DEFER_INSTANCES_UAV];
        deferred[d] = packItem(instance, view);
    }
    const uint s = WaveActiveCountBits(visible);
    if (WaveIsFirstLane() && s > 0) state.InterlockedAdd(4 * VS_STAT_INSTANCES, s);
}
