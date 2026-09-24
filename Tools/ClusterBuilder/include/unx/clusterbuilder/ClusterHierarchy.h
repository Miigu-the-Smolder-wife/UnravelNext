#pragma once
// V-internal GPU records of the cluster LOD hierarchy (ARCHITECTURE 2.1). Owner: V. Mirror: Native/Render/Passes/
// Visibility/ClusterHierarchy.hlsli. Installed with GpuScene::setClusters as named buffers (ClusterData::Named) and read
// only by V's culling kernels; other tracks use gpu::Cluster / gpu::LodLevel (GpuSceneLayout.h).
//
// Selection rule (object-space errors, projected to pixels per view): a cluster is drawn when
//   projected(parent group sphere, parent error) > threshold      -- tested on the hierarchy node of its group
//   projected(own LOD sphere, own error)         <= threshold     -- tested per cluster (own error 0 = source geometry)
// Parent spheres enclose child spheres and parent errors are >= child errors, so the cut is consistent: exactly one
// of a group and its simplification is drawn, without cracks or overlaps.
#include "unx/core/Math.h"

#include <cstdint>

namespace unx::clusterbuilder::gpu
{
// "clusterNodes": per-mesh forest (one tree per DAG depth), nodes of a mesh are contiguous.
struct ClusterNode  // 32 B
{
    float4 lodSphere;    // object space centre, radius: encloses every group sphere (and all geometry) below
    float lodError;      // max group error below (FLT_MAX when a terminal group is below)
    uint32_t first;      // internal node: first child node (global index); leaf: first cluster of its group (global)
    uint32_t count;      // children or clusters
    uint32_t leaf;       // 1 = leaf (one group), 0 = internal
};
static_assert(sizeof(ClusterNode) == 32);

// "meshClusterRoots": one per scene mesh.
struct MeshClusterRoots  // 8 B
{
    uint32_t nodeOffset; // the mesh's per-depth tree roots are nodes [nodeOffset, nodeOffset + rootCount)
    uint32_t rootCount;
};
static_assert(sizeof(MeshClusterRoots) == 8);

// "clusterLodSpheres": float4 per cluster (global index): the sphere its own error is measured on (the group it was
// simplified from; source clusters: their own bounds with error 0). The error itself is gpu::Cluster::lodError.
} // namespace unx::clusterbuilder::gpu
