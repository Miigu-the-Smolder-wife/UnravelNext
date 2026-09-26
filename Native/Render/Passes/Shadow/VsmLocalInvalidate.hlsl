// unx-kernel: cs_6_6 main
// Dirty rule (a) for the local lights: every moved caster (VsmMoved: bit 31 of its entry) makes the resident pages under
// its old and new bounding spheres stale, on every face and mip of every raster-active local light whose range meets
// them. One group per (moved caster, active light); its 64 threads split the (face, mip) page rectangles. The sphere's
// face footprint is the tangent-space box of its 8 bounding-cube corners (conservative); faces it does not reach in
// front of the light are skipped.
// P[0].x page table UAV (raw), P[0].y moved list SRV (raw: count, then (instance, mask) pairs), P[0].z local lights SRV,
// P[0].w active slots SRV; P[1].x active light count. Frame constants (scene buffers).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

void markSphere(RWByteAddressBuffer table, VsmLocalLight l, uint slot, float3 centre, float radius, uint lane)
{
    const float3 d = centre - l.position;
    if (length(d) - radius > l.farM) return;
    [loop] for (uint face = 0; face < 6; ++face)
    {
        float3 right, up, axis;
        vsmCubeBasis(face, right, up, axis);
        float2 lo = 3.0e38, hi = -3.0e38;
        bool front = false;
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            const float3 p = d + radius * float3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1);
            const float z = dot(p, axis);
            if (z <= 1e-4)
            {
                // A corner behind the face plane: the sphere spans the face's edge region; take the whole face side.
                lo = min(lo, float2(-1, -1));
                hi = max(hi, float2(1, 1));
                continue;
            }
            front = true;
            const float2 t = float2(dot(p, right), dot(p, up)) / z;
            lo = min(lo, t);
            hi = max(hi, t);
        }
        if (!front || any(lo > 1) || any(hi < -1)) continue;
        lo = clamp(lo, -1, 1);
        hi = clamp(hi, -1, 1);
        [loop] for (uint mip = 0; mip < VSM_LOCAL_MIPS; ++mip)
        {
            // Tangent x -> texel x = (x/2 + 1/2) res; tangent y -> texel y = (1/2 - y/2) res.
            const uint res = vsmLocalRes(mip), n = 1u << mip;
            const uint2 p0 = min(uint2(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * res) >> VSM_PAGE_SHIFT, n - 1);
            const uint2 p1 = min(uint2(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * res) >> VSM_PAGE_SHIFT, n - 1);
            const uint2 size = p1 - p0 + 1;
            [loop] for (uint i = lane; i < size.x * size.y; i += 64)
            {
                const uint s = vsmLocalSlot(slot, face, mip, p0 + uint2(i % size.x, i / size.x));
                const uint2 e = table.Load2(s * 8);
                if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == l.generation && (e.x & VSM_FLAG_STALE) == 0) table.InterlockedOr(s * 8, VSM_FLAG_STALE);
            }
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer moved = ResourceDescriptorHeap[P[0].y];
    if (group.x >= moved.Load(0) || group.y >= P[1].x) return;
    const uint2 entry = moved.Load2(4 + group.x * 8);
    if ((entry.y & 0x80000000u) == 0) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint> active = ResourceDescriptorHeap[P[0].w];
    const uint slot = active[group.y];
    const VsmLocalLight l = lights[slot];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float scale = length(inst.objectToWorld[0].xyz);
    const float radius = (mesh.boundsSphere.w + windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w)) * scale;
    markSphere(table, l, slot, transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), radius, lane);
    // A motion break (teleport, restore; v1.45) zeroes prev: the pages hold it at breakCentre, the previous frame's place.
    const float3 before = (inst.flags & INSTANCE_MOTION_BREAK) != 0 ? inst.breakCentre : transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz);
    markSphere(table, l, slot, before, radius, lane);
}
