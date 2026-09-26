// unx-kernel: cs_6_6 main
// Skinned instance bounds (C3): one group per skinned instance slot (VisibilityTrack: the scene's skinned instances with
// skin data, sorted by instance). Linear blend skinning puts a vertex at sum_k w_k P_k v (Deformation.hlsli skin, P = the
// palette's jointToModel x inverseBind), a weighted mean of points P_k v, each inside the palette image of the bind-space
// sphere of the vertices joint k influences; the mean lies in the box of those images. So the object-space box of the
// transformed joint spheres (radius x a bound of the joint's largest stretch) bounds every skinned vertex, up to the
// 16-bit weight quantisation (sum of the four weights within 1 +- 2/65535: the box grows by that fraction of its
// farthest point from the origin). Its circumscribed sphere goes to world space like worldSphere. Written for the
// current and the previous palette and transform (phase 1 HiZ uses the previous). A mesh with more than
// SKIN_MAX_JOINTS influencing joints gets radius -1 (unbounded: the instance stays always visible, as before C3).
// Root constants: P[0] slots SRV (uint4: instance, first sphere, sphere count, 0), joint spheres SRV (SkinJointSphere),
//                 bounds UAV (float4 x 2 per slot: current, previous), slot count.
#include "Passes/Visibility/VisibilityCommon.hlsli"

#define SKIN_MAX_JOINTS 1024u
#define SKIN_JOINT_ORIGIN 0xFFFFFFFFu  // a vertex with no weight stays at the object origin

struct SkinJointSphere
{
    float4 sphere;  // bind (mesh object) space centre, radius
    uint joint;     // palette joint, or SKIN_JOINT_ORIGIN
    uint3 pad;
};

// Upper bound of the largest stretch |A x| / |x| of the joint's 3 x 3 part: sqrt of the largest absolute row sum of
// A^T A (Gershgorin bound of its largest eigenvalue); exact for rotations with uniform scale.
float stretchBound(float3x4 m)
{
    const float3 c0 = float3(m[0].x, m[1].x, m[2].x), c1 = float3(m[0].y, m[1].y, m[2].y), c2 = float3(m[0].z, m[1].z, m[2].z);
    const float a00 = dot(c0, c0), a11 = dot(c1, c1), a22 = dot(c2, c2), a01 = abs(dot(c0, c1)), a02 = abs(dot(c0, c2)), a12 = abs(dot(c1, c2));
    return sqrt(max(a00 + a01 + a02, max(a01 + a11 + a12, a02 + a12 + a22)));
}

groupshared float3 gLo[2][64];
groupshared float3 gHi[2][64];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint slot = gid.x;
    if (slot >= P[0].w) return;  // uniform per group
    StructuredBuffer<uint4> slots = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<SkinJointSphere> spheres = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float4> bounds = ResourceDescriptorHeap[P[0].z];
    const uint4 sl = slots[slot];
    const GpuInstance inst = loadInstance(sl.x);
    float3 lo[2] = { (float3)1e30, (float3)1e30 }, hi[2] = { (float3)-1e30, (float3)-1e30 };
    const uint count = min(sl.z, SKIN_MAX_JOINTS);
    [loop] for (uint j = lane; j < count; j += 64)  // at most SKIN_MAX_JOINTS / 64 iterations
    {
        const SkinJointSphere js = spheres[sl.y + j];
        [unroll] for (uint t = 0; t < 2; ++t)
        {
            float3 c = js.sphere.xyz;
            float r = js.sphere.w + inst.morphRadius;  // C4: blend shapes move bind-space vertices by <= morphRadius before skinning
            if (js.joint != SKIN_JOINT_ORIGIN)
            {
                const float3x4 m = loadJoint(t == 0 ? g_bonePalette : g_prevBonePalette, inst.bonePalette + js.joint);
                c = mul(m, float4(c, 1));
                r *= stretchBound(m);
            }
            lo[t] = min(lo[t], c - r);
            hi[t] = max(hi[t], c + r);
        }
    }
    [unroll] for (uint t = 0; t < 2; ++t)
    {
        gLo[t][lane] = lo[t];
        gHi[t][lane] = hi[t];
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            [unroll] for (uint t = 0; t < 2; ++t)
            {
                gLo[t][lane] = min(gLo[t][lane], gLo[t][lane + stride]);
                gHi[t][lane] = max(gHi[t][lane], gHi[t][lane + stride]);
            }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane != 0) return;
    const float scale = instanceScale(inst);
    [unroll] for (uint t = 0; t < 2; ++t)
    {
        const float3 l = gLo[t][0], h = gHi[t][0];
        const float3 c = 0.5 * (l + h);
        float r = 0.5 * length(h - l);
        r += (2.0 / 65535.0) * (length(c) + r);  // weight quantisation
        const float3 w = t == 0 ? transformPoint(inst.objectToWorld, c) : transformPoint(inst.prevObjectToWorld, c);
        bounds[2 * slot + t] = sl.z > SKIN_MAX_JOINTS || sl.z == 0 ? float4(w, -1) : float4(w, r * scale);
    }
}
