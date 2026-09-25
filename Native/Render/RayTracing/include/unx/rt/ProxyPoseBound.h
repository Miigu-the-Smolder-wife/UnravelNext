#pragma once
// Error of a skinned mesh's RT proxy cut in the current pose (R-internal, RayScene).
//
// A cut's LOD error is measured in the bind pose; skinning moves the source mesh and the cut differently wherever a cut
// triangle spans joints (a coarse triangle across a bent elbow cuts the corner). Bound for linear blend skinning
// (palette M_i = [A_i | t_i], object space): for a source point x and a cut point q that correspond in the bind pose,
// each an affine combination of mesh vertices x_k with weights beta_k (vertex joint weights w_ki, blended w_i),
//
//   |x' - q'| <= s |x - q| + sum_i [ alpha_i (|d_i| |q - o_i| + h_i) + beta_i |d_i| ]
//
//   d_i     = w_i(x) - w_i(q)                                  joint i's blended weight difference
//   h_i     = sum over both combinations of beta_k |w_ki - w_i| |x_k - y|   (y = x or q: the corners' weight spread)
//   alpha_i = ||A_i - A_r||_2, beta_i = |M_i(o_i) - M_r(o_i)|     joint i against a reference joint r at its centre o_i
//   s       = max_i ||A_i||
//
// (sum_i d_i = 0 lets every joint term be taken relative to joint r; sum_k beta_k (x_k - y) = 0 lets the corner terms
// be taken relative to the blended linear part.) The pairs are sampled both ways at load time: every source vertex
// with its closest cut point (geometry the cut loses) and each cut triangle's corners, edge midpoints and centroid with
// its closest source point (geometry the cut adds). Per cut and joint the maxima K1_i = max(|d_i| |q - o_i| + h_i) and
// K2_i = max |d_i| are kept, with e = max |x - q|; per frame the bound is s e + sum_i (alpha_i K1_i + beta_i K2_i), a few
// operations per joint on the CPU palette (GpuScene::palette, INTERFACES v1.34).
#include "unx/core/Math.h"

#include <cstdint>
#include <span>
#include <vector>

namespace unx::scene
{
struct Mesh;
}

namespace unx::render::rt
{
struct ProxyPoseCoefficients
{
    float bindError = 0;           // max |x - q| over the sampled pairs (object space)
    std::vector<float> k1, k2;     // per joint (empty: rigid mesh, the bind error alone)
};

struct ProxyPoseSkeleton
{
    uint32_t reference = 0;        // joint r: the one with the largest total weight over the mesh
    std::vector<float3> centres;   // o_i: weight-averaged bind position of the joint's vertices
};

// Joint centres and the reference joint of a skinned mesh.
ProxyPoseSkeleton proxyPoseSkeleton(const scene::Mesh& mesh);

// Coefficients of a cut given as a triangle list of mesh vertex indices.
ProxyPoseCoefficients proxyPoseCoefficients(const scene::Mesh& mesh, const ProxyPoseSkeleton& skeleton, std::span<const uint32_t> cutIndices);

// Per frame: per joint (alpha_i, beta_i) and s from a palette of 3 float4 rows per joint (jointToModel x inverseBind).
struct ProxyPoseTerms
{
    float s = 1;
    std::vector<float> alpha, beta;
};
void proxyPoseTerms(const ProxyPoseSkeleton& skeleton, std::span<const float4> palette, ProxyPoseTerms& out);

// The cut's error bound in this pose (object space).
float proxyPoseError(const ProxyPoseCoefficients& c, const ProxyPoseTerms& t);
} // namespace unx::render::rt
