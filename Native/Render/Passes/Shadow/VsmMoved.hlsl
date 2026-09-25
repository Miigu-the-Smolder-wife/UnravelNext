// unx-kernel: cs_6_6 main
// Dirty rule (a) of ARCHITECTURE 2.3, first half, with the texel test of rule (b): a level re-renders a caster's pages
// only when the caster has moved by more than windTexels texels of that level since the level last rendered them. Per
// instance whose transform or deformation revision changed, the frame's displacement bound of its bounding sphere,
//   rigid: |(M - M') c| + ||A - A'||_F r      (M, M' this and the previous frame's object-to-world, A the 3 x 3 part)
//   skin:  max over its joints of |(J - J') c| + ||J_A - J'_A||_F r, in object space, times the world scale
// accumulates per (instance, level); where the sum reaches the level's threshold the level's bit is set and the sum
// restarts (VsmInvalidate marks the pages and moves the level's anchor). Sub-texel motion therefore never re-renders a
// coarse page, and accumulated motion does once it matters. Newly seen instances and scene invalidation reset.
// Every moved caster is listed with bit 31 set (the local lights' pages, VsmLocalInvalidate), with or without levels.
// P[0].x last revisions UAV (uint2 per instance), P[0].y moved list UAV (raw: count, then (instance, level mask) pairs),
// P[0].z instance count, P[0].w motion state UAV (float4 per instance x level: anchor centre, accumulated displacement)
// P[1].x joint counts SRV (uint per instance; 0 = rigid), P[1].y VSM constants CBV. Frame constants (scene buffers).
#include "Bindless.hlsli"
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

float frobenius(float3x3 m) { return sqrt(dot(m[0], m[0]) + dot(m[1], m[1]) + dot(m[2], m[2])); }

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    RWStructuredBuffer<uint2> lastRevision = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> motion = ResourceDescriptorHeap[P[0].w];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].y];
    const GpuInstance inst = loadInstance(i);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float3 centre = transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz);
    const uint2 rev = uint2(inst.transformRevision, inst.deformRevision);
    const uint2 last = lastRevision[i];
    if (c.sceneInvalidate != 0)
    {
        // Every page re-renders: every level's anchor is here now.
        lastRevision[i] = rev;
        [loop] for (uint k = 0; k < VSM_LEVELS; ++k) motion[i * VSM_LEVELS + k] = float4(centre, 0);
        return;
    }
    if (all(last == rev)) return;
    lastRevision[i] = rev;
    if ((inst.flags & INSTANCE_CAST_SHADOW) == 0) return;
    const float3 prevCentre = transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz);
    const float r = mesh.boundsSphere.w;
    const float3x3 a = float3x3(inst.objectToWorld[0].xyz, inst.objectToWorld[1].xyz, inst.objectToWorld[2].xyz);
    const float3x3 ap = float3x3(inst.prevObjectToWorld[0].xyz, inst.prevObjectToWorld[1].xyz, inst.prevObjectToWorld[2].xyz);
    float d = length(centre - prevCentre) + frobenius(a - ap) * r;
    StructuredBuffer<uint> jointCounts = ResourceDescriptorHeap[P[1].x];
    const uint joints = inst.bonePalette == UNX_NONE ? 0u : jointCounts[i];
    float skin = 0;
    [loop] for (uint j = 0; j < joints; ++j)
    {
        const float3x4 m = loadJoint(g_bonePalette, inst.bonePalette + j), mp = loadJoint(g_prevBonePalette, inst.bonePalette + j);
        const float3x4 dm = m - mp;
        skin = max(skin, length(mul(dm, float4(mesh.boundsSphere.xyz, 1))) + frobenius((float3x3)dm) * r);
    }
    d += skin * length(inst.objectToWorld[0].xyz);
    const bool firstSeen = all(last == 0);
    uint mask = 0;
    [loop] for (uint k = 0; k < VSM_LEVELS; ++k)
    {
        float4 s = motion[i * VSM_LEVELS + k];
        if (firstSeen) s = float4(prevCentre, 0);  // never moved before: its pages hold it where it was last frame
        s.w += d;
        if (firstSeen || s.w >= vsmTexel(k) * c.windTexels)
        {
            mask |= 1u << k;
            s.w = 0;
        }
        motion[i * VSM_LEVELS + k] = s;
    }
    RWByteAddressBuffer moved = ResourceDescriptorHeap[P[0].y];
    uint at;
    moved.InterlockedAdd(0, 1, at);
    moved.Store2(4 + at * 8, uint2(i, mask | 0x80000000u));
}
