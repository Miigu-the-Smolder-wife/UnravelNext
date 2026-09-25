#pragma once
// Error of a skinned mesh's RT proxy cut in the current pose (R-internal, RayScene).
//
// A cut's LOD error is measured in the bind pose; skinning moves the source mesh and the cut differently wherever a cut
// triangle spans joints (a coarse triangle across a bent elbow cuts the corner). Bound for linear blend skinning
// (palette M_i = [A_i | t_i], object space): for a source point x and a cut point q that correspond in the bind pose,
// each an affine combination of mesh vertices x_k with weights beta_k (vertex joint weights w_ki, blended w_i),
//
//   |x' - q'| <= s |x - q| + sum_{i != r} [ alpha_ir (|d_i| |q - o_i| + h_i) + beta_ir |d_i| ]
//
//   d_i      = w_i(x) - w_i(q)                                  joint i's blended weight difference
//   h_i      = sum over both combinations of beta_k |w_ki - w_i| |x_k - y|   (y = x or q: the corners' weight spread)
//   r        = the pair's dominant joint (largest blended weight at q): any joint works, a local one keeps it tight
//   alpha_ir = ||A_i - A_r||_2, beta_ir = |M_i(o_i) - M_r(o_i)|  joint i against r, at joint i's centre o_i
//   s        = max_i ||A_i||
//
// (sum_i d_i = 0 lets every joint term be taken relative to r; sum_k beta_k (x_k - y) = 0 lets the corner terms be
// taken relative to the blended linear part.) The pairs are sampled both ways at load time: every source vertex with
// its closest cut point (geometry the cut loses) and each cut triangle's corners, edge midpoints and centroid with its
// closest source point (geometry the cut adds). A pair's joints differ in weight only across neighbouring joints, so
// alpha_ir is a local relative rotation (small), not a joint's rotation against one global joint (large along a chain).
// Per cut, per (joint i, reference r): K1 = max(|d_i| |q - o_i| + h_i), K2 = max |d_i| over the pairs whose reference
// is r, with e = max |x - q|. Per frame the bound is s e + max over r of sum_i (alpha_ir K1 + beta_ir K2): the pairs of
// one reference share one sum, and every pair is in exactly one. alpha_ir uses min(||D||_F, sqrt(||D||_1 ||D||_inf)) >=
// ||D||_2 (cheap and exact for small rotations). A few operations per (i, r) on the CPU palette (GpuScene::palette,
// INTERFACES v1.34).
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
    float bindError = 0;  // max |x - q| over the sampled pairs (object space)
    struct Term
    {
        uint32_t pair;       // index into ProxyPoseSkeleton::pairs
        uint32_t reference;  // the pair's reference joint (terms are grouped by it)
        float k1, k2;
    };
    std::vector<Term> terms;  // sorted by reference joint (empty: rigid mesh, the bind error alone)
};

struct ProxyPoseSkeleton
{
    std::vector<float3> centres;                          // o_i: weight-averaged bind position of the joint's vertices
    std::vector<std::pair<uint32_t, uint32_t>> pairs;     // (joint i, reference r) of every cut's terms (shared by the cuts)
};

// Joint centres of a skinned mesh (no pairs yet: proxyPoseCoefficients registers them).
ProxyPoseSkeleton proxyPoseSkeleton(const scene::Mesh& mesh);

// Coefficients of a cut given as a triangle list of mesh vertex indices; registers its (joint, reference) pairs.
ProxyPoseCoefficients proxyPoseCoefficients(const scene::Mesh& mesh, ProxyPoseSkeleton& skeleton, std::span<const uint32_t> cutIndices);

// Per frame: s and per registered pair (alpha_ir, beta_ir) from a palette of 3 float4 rows per joint
// (jointToModel x inverseBind).
struct ProxyPoseTerms
{
    float s = 1;
    std::vector<float> alpha, beta;  // per ProxyPoseSkeleton::pairs entry
};
void proxyPoseTerms(const ProxyPoseSkeleton& skeleton, std::span<const float4> palette, ProxyPoseTerms& out);

// Skin-aware RT proxy cuts of a skinned mesh, finest first: per level a triangle list per submesh (mesh vertex indices).
// Cuts from V's cluster LOD ignore skin weights, so their triangles span joints and cut the corner of a bent limb (measured:
// 4-7 cm in a pose for cuts of 0.03-3 mm bind-pose error). Here every level is simplified from the source mesh with
// meshopt_simplifyWithAttributes, the attribute being the vertex's skin-weight centroid sum_i w_i o_i (joint centres
// blended by its weights): collapsing an edge whose ends move with different joints costs about |dw| x the joints'
// spacing, so triangles do not stretch across a bendable region, while edges within one joint's region collapse freely.
// 'attributeWeight' scales that cost against the positional error. Submesh borders are locked. The first level is the
// source itself when it has at most 'budget' triangles, else a cut of 'budget'; each next level halves the target until
// the simplifier cannot reduce further or 16 triangles remain. Each cut's error in a pose is still bounded by
// proxyPoseCoefficients / proxyPoseError: the attribute shapes the cut, it does not certify it.
std::vector<std::vector<std::vector<uint32_t>>> skinAwareCuts(const scene::Mesh& mesh, const ProxyPoseSkeleton& skeleton, uint32_t budget, float attributeWeight);

// The cut's error bound in this pose (object space).
float proxyPoseError(const ProxyPoseCoefficients& c, const ProxyPoseTerms& t);
} // namespace unx::render::rt
