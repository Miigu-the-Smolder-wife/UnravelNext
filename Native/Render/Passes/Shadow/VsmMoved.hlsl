// unx-kernel: cs_6_6 main
// Dirty rule (a) of ARCHITECTURE 2.3, first half: instances whose transform or deformation (skin) revision changed since
// the last frame and that cast shadows are appended to a list; VsmInvalidate marks their pages. Revisions of every
// instance are remembered. Work is O(instances) with one comparison each; the page work is O(moved instances).
// P[0].x last revisions UAV (uint2 per instance), P[0].y moved list UAV (raw: count, then instance indices),
// P[0].z instance count. Frame constants of the main view (scene buffers).
#include "Bindless.hlsli"
#include "Scene.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    RWStructuredBuffer<uint2> lastRevision = ResourceDescriptorHeap[P[0].x];
    const GpuInstance inst = loadInstance(i);
    const uint2 rev = uint2(inst.transformRevision, inst.deformRevision);
    if (all(lastRevision[i] == rev)) return;
    lastRevision[i] = rev;
    if ((inst.flags & INSTANCE_CAST_SHADOW) == 0) return;
    RWByteAddressBuffer moved = ResourceDescriptorHeap[P[0].y];
    uint at;
    moved.InterlockedAdd(0, 1, at);
    moved.Store(4 + at * 4, i);
}
