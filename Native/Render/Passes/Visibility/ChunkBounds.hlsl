// unx-kernel: cs_6_6 main
// Instance chunk bounds (C3, VisibilityCommon.hlsli CullScene): one thread per chunk, the sphere around its members' world
// spheres without wind (the box of the member spheres, then its circumscribed sphere) and, in pad0, the largest member
// wind inflation per (m/s)^2 (windOffsetScale x instance scale): the cull kernels add it x this frame's wind speed^2, so
// a chunk bounds every member's worldSphere at any wind; in pad1 the members' largest world radius without wind (the
// views that draw proxies skip a chunk whose every member is under their smallest instance: chunkBelowView). Runs at each
// scene revision (chunk members are static). Each thread visits at most CHUNK_INSTANCES members (bounded dispatch).
// Root constants: P[0] chunks UAV (CullChunk), chunk instances SRV (uint), chunk count, unused.
#include "Passes/Visibility/VisibilityCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[0].z) return;
    RWStructuredBuffer<CullChunk> chunks = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint> members = ResourceDescriptorHeap[P[0].y];
    CullChunk ch = chunks[id];
    float3 lo = 1e30, hi = -1e30;
    float wind = 0, largest = 0;
    const uint count = min(ch.count, CHUNK_INSTANCES);
    [loop] for (uint k = 0; k < count; ++k)
    {
        const GpuInstance inst = loadInstance(members[ch.first + k]);
        const GpuMesh mesh = loadMesh(inst.mesh);
        const float scale = instanceScale(inst);
        const float3 c = transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz);
        const float r = (mesh.boundsSphere.w + inst.morphRadius) * scale;  // C4 morph bound included
        lo = min(lo, c - r);
        hi = max(hi, c + r);
        largest = max(largest, r);
        wind = max(wind, windOffsetScale(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w) * scale);
    }
    ch.sphere = count > 0 ? float4(0.5 * (lo + hi), 0.5 * length(hi - lo)) : float4(0, 0, 0, -1);
    ch.pad0 = asuint(wind);
    ch.pad1 = asuint(largest);
    chunks[id] = ch;
}
