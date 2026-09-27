// Previous-frame position of a screen surface point (R-internal: the GI screen histories). The exact motion of V's
// surface, as ReflectionAccumulate.hlsl takes it: the pixel's vis id -> visible cluster -> instance and triangle, the
// point's barycentric coordinates in the triangle's current vertices, the same coordinates in the previous-tick vertices
// (Deformation.hlsli deformVertex: rigid transforms, skinning, morphs and wind alike). A pixel without a vis id (sky, or
// frames without V's vis buffer) keeps its position: the history's plane test then decides.
#ifndef UNX_GI_SCREEN_HISTORY_HLSLI
#define UNX_GI_SCREEN_HISTORY_HLSLI
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Common/Deformation.hlsli"

// Minimal rotation taking unit a to unit b, applied to v (Rodrigues; a = -b keeps v).
float3 giRotateBetween(float3 a, float3 b, float3 v)
{
    const float3 k = cross(a, b);
    const float c = dot(a, b);
    if (c < -0.9999) return v;
    return v * c + cross(k, v) + k * (dot(k, v) / (1 + c));
}

// p, n: the point and its normal this frame; prevP, prevN: the same surface point and normal one tick ago. instance: the
// scene instance + 1 (0: no vis id).
void giPreviousSurface(uint visIdSrv, uint visibleClustersSrv, uint2 pixel, float3 p, float3 n, out float3 prevP, out float3 prevN, out uint instance)
{
    prevP = p;
    prevN = n;
    instance = 0;
    if (visIdSrv == UNX_NONE) return;
    Texture2D<uint> visIds = ResourceDescriptorHeap[visIdSrv];
    const uint visId = visIds.Load(int3(pixel, 0));
    if (visId == VIS_NONE) return;
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    instance = vc.instance + 1;
    const GpuInstance inst = loadInstance(vc.instance);
    if (deformInstanceStill(inst)) return;  // the point itself (no vertex loads)
    const GpuMesh mesh = loadMesh(inst.mesh);
    const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
    const DeformedVertex d0 = deformVertex(inst, mesh, tri.x), d1 = deformVertex(inst, mesh, tri.y), d2 = deformVertex(inst, mesh, tri.z);
    const float3 e1 = d1.world - d0.world, e2 = d2.world - d0.world, q = p - d0.world;
    const float3 ng = cross(e1, e2);
    const float area2 = dot(ng, ng);
    if (!(area2 > 1e-20)) return;
    const float b1 = dot(cross(q, e2), ng) / area2, b2 = dot(cross(e1, q), ng) / area2;
    // the point's height above the triangle's plane (the G-buffer point is on it up to rounding) carried along its normal
    const float h = dot(q, ng) / sqrt(area2);
    const float3 ngPrev = cross(d1.prevWorld - d0.prevWorld, d2.prevWorld - d0.prevWorld);
    const bool prevValid = dot(ngPrev, ngPrev) > 1e-20;
    prevP = d0.prevWorld + (d1.prevWorld - d0.prevWorld) * b1 + (d2.prevWorld - d0.prevWorld) * b2 + (prevValid ? normalize(ngPrev) * h : 0);
    if (prevValid) prevN = normalize(giRotateBetween(normalize(ng), normalize(ngPrev), n));
}

// Continuous pixel coordinates (pixel centres at + 0.5) of a world point in the previous frame's view; false behind it.
bool giPreviousPixel(float3 prevP, float2 size, out float2 prevPixel, out float prevDepth)
{
    const float4 clip = mul(g_prevViewProj, float4(prevP, 1));
    prevDepth = clip.w;
    prevPixel = 0;
    if (!(clip.w > 0)) return false;
    const float2 ndc = clip.xy / clip.w;
    prevPixel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size;
    return true;
}
#endif
