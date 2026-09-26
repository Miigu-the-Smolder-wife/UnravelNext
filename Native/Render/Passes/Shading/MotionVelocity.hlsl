// unx-kernel: cs_6_6 main
// Motion blur (A5; COVERAGE 14.12 (2b), MotionBlur.cpp), screen velocity per pixel: v = the pixel centre - where the
// surface point under it was in the previous rendered frame (pixels per frame interval). The point is the pixel-centre
// ray's hit on the deformed triangle the vis buffer names (the plane intersection of mSurfaceFromVertices); its previous
// position is the same barycentric blend of the three vertices' previous deformed positions (previous transform, bone
// palette and wind time: deformVertex's prevWorld), projected with the previous view-projection. The sky: the direction at
// infinity under the previous view (the camera's rotation only). RG16F. With the rotation stage (MotionRotation.hlsl,
// P[1].z = 1) the camera's rotation is taken out: the velocity written is the residual v - v_rot, v_rot(p) = p - the pixel of
// Q^T d_p (the previous view direction of what p sees now, under the rotation alone).
// P[0] = { vis id SRV, visible clusters SRV, velocity UAV, 0 }, P[1] = { width, height, rotation stage, 0 },
// P[2..4] = asfloat rows of Q^T (view space, xyz + 0); frame constants of the view.
#include "Bindless.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

float2 pixelOf(float4 clip) { const float2 ndc = clip.xy / clip.w; return float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[1].xy)) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float2> velocity = ResourceDescriptorHeap[P[0].z];
    const float2 pixel = float2(id) + 0.5f;
    float3 D, Dx, Dy;
    mPixelRay(pixel, D, Dx, Dy);
    const uint visId = vis.Load(int3(id, 0));
    float4 prevClip;
    if (visId == VIS_NONE)
    {
        prevClip = mul(g_prevViewProj, float4(D, 0));  // a direction: the previous view's rotation only
    }
    else
    {
        const GpuVisibleCluster vc = loadVisibleCluster(P[0].y, visVisibleCluster(visId));
        const GpuInstance inst = loadInstance(vc.instance);
        const GpuCluster c = loadCluster(vc.cluster);
        const GpuMesh mesh = loadMesh(inst.mesh);
        const uint3 tri = loadClusterTriangle(c, visTriangle(visId));
        const DeformedVertex d0 = deformVertex(inst, mesh, tri.x), d1 = deformVertex(inst, mesh, tri.y), d2 = deformVertex(inst, mesh, tri.z);
        const float3 r0 = d0.world - g_cameraPosition;
        const float3 e1 = d1.world - d0.world, e2 = d2.world - d0.world;
        const float3 n = cross(e1, e2);
        const float nD = dot(n, D);
        float3 prev;
        if (abs(nD) > 1e-20f && dot(n, n) > 1e-30f)
        {
            const float t = dot(n, r0) / nD;
            const float3 r = t * D - r0;
            const float inv = 1.0f / dot(n, n);
            const float b1 = dot(n, cross(r, e2)) * inv, b2 = dot(n, cross(e1, r)) * inv;
            prev = d0.prevWorld + (d1.prevWorld - d0.prevWorld) * b1 + (d2.prevWorld - d0.prevWorld) * b2;
        }
        else
        {
            prev = (d0.prevWorld + d1.prevWorld + d2.prevWorld) / 3.0f;  // edge-on triangle: its centre
        }
        prevClip = mul(g_prevViewProj, float4(prev, 1));
    }
    // behind the previous camera (w <= 0): no previous screen position; the pixel keeps no motion
    float2 v = prevClip.w > 1e-6f ? pixel - pixelOf(prevClip) : float2(0, 0);
    if (P[1].z != 0)
    {
        // the view-space direction of this pixel, then where the rotation alone had it in the previous frame
        const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
        const float3 d = float3((ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0], (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1], -1);
        const float3 q = float3(dot(asfloat(P[2].xyz), d), dot(asfloat(P[3].xyz), d), dot(asfloat(P[4].xyz), d));
        if (q.z < -1e-6f)
        {
            const float2 qn = float2(q.x * g_proj[0][0] / -q.z - g_proj[0][2] + g_proj[0][3], q.y * g_proj[1][1] / -q.z - g_proj[1][2] + g_proj[1][3]);
            v -= pixel - float2((qn.x + 1) * 0.5f * g_viewWidth, (1 - qn.y) * 0.5f * g_viewHeight);
        }
    }
    velocity[id] = all(isfinite(v)) ? v : float2(0, 0);
}
