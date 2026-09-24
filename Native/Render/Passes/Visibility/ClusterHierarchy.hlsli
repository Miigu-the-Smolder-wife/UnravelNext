// Mirror of Tools/ClusterBuilder/include/unx/clusterbuilder/ClusterHierarchy.h (V internal). Owner: V.
#ifndef UNX_CLUSTER_HIERARCHY_HLSLI
#define UNX_CLUSTER_HIERARCHY_HLSLI

struct ClusterNode  // 32 B
{
    float4 lodSphere;  // object space: encloses every group sphere (and all geometry) below
    float lodError;    // max group error below (>= 3.4e38: a terminal group is below)
    uint first;        // internal node: first child node; leaf: first cluster of its group
    uint count;        // children or clusters
    uint leaf;         // 1 = leaf (one group)
};

struct MeshClusterRoots  // 8 B
{
    uint nodeOffset;   // roots are nodes [nodeOffset, nodeOffset + rootCount)
    uint rootCount;
};

#endif
