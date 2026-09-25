#pragma once
// Minimum feature width of a cluster (ARCHITECTURE 2.1 bands A/B/C). Owner: V.
//
// A cluster's triangles split into pieces (connected through shared edges of welded positions). For each piece:
//   thickness    = median distance from the piece's largest triangles along -normal to the opposite wall of the same
//                  connected mesh component (tube -> diameter, slab or box -> its thickness; open sheets -> none)
//   sheet width  = diameter of the largest disk inside the sheet that contains the piece's vertices (medial radius,
//                  computed once per mesh on the source triangles): every source triangle gets the centred disk
//                  r(t) = largest distance from its samples (vertices, edge midpoints, centroid) to the mesh boundary
//                  (edges used by one triangle) of its component, then R(t) = max r(t') over the disks that contain
//                  its centroid; a vertex takes the max over its triangles (strip of width w -> w, 5 cm leaf -> 5 cm,
//                  terrain -> its extent even at its border)
//   width        = thickness when there is an opposite wall (solid: the silhouette is never narrower, from any
//                  direction), else the sheet width (flat: open sheet, projected width shrinks with the view angle).
// Pieces are cut by cluster borders, but both measures use the whole mesh, so a cluster border is never mistaken for
// a feature. Two cluster values come out:
//   narrowest  its narrowest piece, signed (> 0 solid, < 0 flat): the band A/B/C classification (gpu::Cluster).
//   guard      the smallest component width among its pieces, where a connected component's width is the
//              area-weighted median over its source triangles (thickness, or 2 R): what the LOD guard protects. A
//              corner of a terrain tile or the tip of a wide leaf is locally narrow but is not a thin feature; a blade,
//              a leaf or a wire is thin as a whole.
// The builder also never groups components of very different widths (width classes, ClusterBuilder.cpp), so a blade
// is never simplified in a group whose error suits the terrain around it.
// A narrow part of a wider piece (a leaf stem) is not resolved separately; the P1 census gate (missed subsamples)
// measures what that costs in coverage.
#include "unx/core/Math.h"

#include <cstdint>
#include <vector>

namespace unx::clusterbuilder::detail
{
// Binary BVH over axis-aligned boxes (median split): thickness rays (triangles) and boundary distances (segments).
struct Bvh
{
    struct Node
    {
        float3 lo, hi;
        uint32_t first, count;  // leaf: primitive range in order; internal: count = 0, children first and first + 1
    };
    std::vector<Node> nodes;
    std::vector<uint32_t> order;
    void build(const std::vector<float3>& lo, const std::vector<float3>& hi);
};

class MeshWidthContext
{
public:
    // positions: the mesh's vertices; indices: every triangle of the mesh (all submeshes).
    MeshWidthContext(const std::vector<float3>& positions, const std::vector<uint32_t>& indices);

    struct Width
    {
        float narrowest = 0;  // signed: > 0 solid, < 0 flat sheet of width |w|
        float guard = 0;      // smallest component width among the pieces (unsigned, FLT_MAX = unbounded)
    };
    // Widths of a cluster given as a triangle list of mesh vertex indices.
    Width clusterWidth(const uint32_t* indices, size_t indexCount) const;

    const std::vector<uint32_t>& weld() const { return m_weld; }
    // Width of the connected component a mesh vertex belongs to (FLT_MAX = unbounded).
    float componentWidth(uint32_t vertex) const { return m_componentWidth[m_component[m_weld[vertex]]]; }
    // Connected component (vertex connectivity through welded positions) of a mesh vertex: a canonical vertex id.
    uint32_t component(uint32_t vertex) const { return m_component[m_weld[vertex]]; }

private:
    float thickness(float3 origin, float3 dir, uint32_t component) const;
    float boundaryDistance(float3 p, uint32_t component) const;
    void computeMedialRadii(std::vector<float>& triangleRadius);
    void computeComponentWidths(const std::vector<float>& triangleRadius);

    const std::vector<float3>& m_positions;
    const std::vector<uint32_t>& m_indices;
    std::vector<uint32_t> m_weld;       // canonical vertex per vertex (same position)
    std::vector<uint32_t> m_component;  // per canonical vertex: connected component (vertex connectivity)
    Bvh m_triangles;
    std::vector<uint32_t> m_segments;   // boundary edges: canonical vertex pairs
    Bvh m_boundary;
    std::vector<float> m_vertexRadius;  // per canonical vertex: largest inscribed disk radius containing it (open sheets)
    std::vector<float> m_componentWidth;  // per component root (canonical vertex id): area-weighted median width
    float m_epsilon = 0;                // ray start offset: 1e-5 of the mesh extent
};

} // namespace unx::clusterbuilder::detail
