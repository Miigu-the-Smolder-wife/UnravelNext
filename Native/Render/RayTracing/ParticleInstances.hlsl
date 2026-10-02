// unx-kernel: cs_6_6 main
// Mesh particles in the ray scene (fx.particles.mesh_in_rays, off by default; RayScene::record "r.as.particles"). The
// mesh particles are scene instances the GPU writes every frame (FxMeshInstances.hlsl, GpuScene::gpuInstanceRange); R's
// dynamic TLAS takes its instance descriptors from the CPU, which does not know them. With the switch on, the frame's
// descriptors are copied into a buffer this kernel can write, and one thread per slot of the GPU instance range writes
// the slot's descriptor after them and its RtInstance record (the hit shaders' way back to the scene instance and the
// mesh's geometry records).
// Every descriptor is checked here before it is written - a wrong one is undefined behaviour in the build:
//   - the slot is live (below the frame's GPU instance count) and not hidden;
//   - its mesh index is inside the table and the table has a BLAS for it (a committed mesh with triangles; a runtime
//     mesh has none here);
//   - its transform's twelve values are finite and below 1e7 in magnitude, its three axes are between 1e-4 and 1e4 long
//     and its determinant is not 0 (no collapsed or exploded instance).
// A slot that fails any of them is written inactive: identity transform, instance mask 0, a null BLAS address (the
// builder skips it); its record's last word is 1 (a written slot's: 0), for a reader that counts them.
// P[0] = { instance descriptors UAV (raw, D3D12_RAYTRACING_INSTANCE_DESC, 64 B), RtInstance records UAV (raw, 16 B),
//          mesh table SRV (StructuredBuffer<uint4>: BLAS address low, high, geometry base, 1 = traceable), mesh count }
// P[1] = { first scene instance of the GPU range, its capacity, descriptor index of slot 0, InstanceID of slot 0 }
// P[2] = { the GPU instance count's element in g_patchData (.x), the instance mask, 0, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"

bool finiteRow(float4 r) { return all(abs(r) < 1e7f); }  // (false for NaN and infinity too)

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x;
    if (slot >= P[1].y) return;
    StructuredBuffer<uint4> counts = ResourceDescriptorHeap[g_patchData];
    const uint live = min(counts[P[2].x].x, P[1].y);
    // inactive unless every check passes
    float4 rows[3] = { float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0) };
    uint mask = 0u, geometryBase = 0u;
    uint2 blas = uint2(0u, 0u);
    bool written = false;
    if (slot < live)
    {
        const GpuInstance inst = loadInstance(P[1].x + slot);
        const float4 r0 = inst.objectToWorld[0], r1 = inst.objectToWorld[1], r2 = inst.objectToWorld[2];
        bool ok = (inst.flags & INSTANCE_HIDDEN) == 0u && inst.mesh < P[0].w;
        uint4 entry = uint4(0u, 0u, 0u, 0u);
        if (ok)
        {
            StructuredBuffer<uint4> meshes = ResourceDescriptorHeap[P[0].z];
            entry = meshes[inst.mesh];
            ok = entry.w == 1u && (entry.x != 0u || entry.y != 0u);
        }
        if (ok) ok = finiteRow(r0) && finiteRow(r1) && finiteRow(r2);
        if (ok)
        {
            const float3 ax = float3(r0.x, r1.x, r2.x), ay = float3(r0.y, r1.y, r2.y), az = float3(r0.z, r1.z, r2.z);
            const float lx = length(ax), ly = length(ay), lz = length(az);
            const float det = dot(ax, cross(ay, az));
            ok = min(lx, min(ly, lz)) > 1e-4f && max(lx, max(ly, lz)) < 1e4f && abs(det) > 1e-12f;
        }
        if (ok)
        {
            rows[0] = r0;
            rows[1] = r1;
            rows[2] = r2;
            mask = P[2].y & 0xFFu;
            blas = entry.xy;
            geometryBase = entry.z;
            written = true;
        }
    }
    RWByteAddressBuffer descs = ResourceDescriptorHeap[P[0].x];
    const uint at = (P[1].z + slot) * 64u;
    descs.Store4(at, asuint(rows[0]));
    descs.Store4(at + 16u, asuint(rows[1]));
    descs.Store4(at + 32u, asuint(rows[2]));
    // InstanceID (24 bits) | mask << 24; hit group offset 0 | flags 0; the BLAS address
    descs.Store4(at + 48u, uint4(((P[1].w + slot) & 0xFFFFFFu) | (mask << 24), 0u, blas.x, blas.y));
    RWByteAddressBuffer records = ResourceDescriptorHeap[P[0].y];
    // RtInstance: scene instance, geometry base, no vertex base, flags 0 (an inactive slot: 1 - no ray reads it)
    records.Store4(slot * 16u, uint4(P[1].x + slot, geometryBase, UNX_NONE, written ? 0u : 1u));
}
