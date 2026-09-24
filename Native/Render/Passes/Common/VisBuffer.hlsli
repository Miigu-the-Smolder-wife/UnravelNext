// Visibility buffer encoding (INTERFACES_KO.md 7.1). Owner: V. Readers: M (material resolve, shading), R, S.
//   vis id  R32_UINT = visibleCluster << 7 | triangle   (visibleCluster < 2^25 per view, triangle < 128)
//           VIS_NONE (0xFFFFFFFF) = no geometry (sky)
//   depth   D32_FLOAT, reversed Z, infinite far (Frame.hlsli linearDepth); cleared to 0
// visibleCluster indexes the per-view VisibleCluster list (instance, cluster) that V writes in the same frame.
#ifndef UNX_VISBUFFER_HLSLI
#define UNX_VISBUFFER_HLSLI
#include "Scene.hlsli"

#define VIS_NONE 0xFFFFFFFFu
#define VIS_TRIANGLE_BITS 7u
#define VIS_TRIANGLE_MASK 0x7Fu

uint packVisId(uint visibleCluster, uint triangle) { return (visibleCluster << VIS_TRIANGLE_BITS) | triangle; }
uint visVisibleCluster(uint visId) { return visId >> VIS_TRIANGLE_BITS; }
uint visTriangle(uint visId) { return visId & VIS_TRIANGLE_MASK; }

GpuVisibleCluster loadVisibleCluster(uint visibleClustersSrv, uint visibleCluster)
{
    StructuredBuffer<GpuVisibleCluster> b = ResourceDescriptorHeap[visibleClustersSrv];
    return b[visibleCluster];
}

// Everything a consumer needs to evaluate the surface under a pixel: the three deformed world-space vertices
// (current and previous tick), their attributes, instance/cluster/material identity.
struct VisTriangle
{
    uint instance, cluster, material;
    float3 world[3];
    float3 prevWorld[3];
    VertexData v[3];
};

#endif
