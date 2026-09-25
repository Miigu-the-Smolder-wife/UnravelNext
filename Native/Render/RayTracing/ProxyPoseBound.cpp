#include "unx/rt/ProxyPoseBound.h"

#include "unx/scene/SceneData.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace unx::render::rt
{
namespace
{

// Closest point of a triangle to p (Ericson, Real-Time Collision Detection 5.1.5) with its barycentrics.
float3 closestOnTriangle(float3 p, float3 a, float3 b, float3 c, float3& bary)
{
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { bary = { 1, 0, 0 }; return a; }
    const float3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { bary = { 0, 1, 0 }; return b; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
    {
        const float v = d1 / (d1 - d3);
        bary = { 1 - v, v, 0 };
        return a + ab * v;
    }
    const float3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { bary = { 0, 0, 1 }; return c; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
    {
        const float w = d2 / (d2 - d6);
        bary = { 1 - w, 0, w };
        return a + ac * w;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        bary = { 0, 1 - w, w };
        return b + (c - b) * w;
    }
    const float denom = 1 / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    bary = { 1 - v - w, v, w };
    return a + ab * v + ac * w;
}

// Uniform grid over a triangle list (mesh vertex indices); nearest point by growing shells of cells.
class TriangleGrid
{
public:
    TriangleGrid(const std::vector<float3>& positions, std::span<const uint32_t> indices) : m_positions(positions), m_indices(indices)
    {
        const uint32_t n = (uint32_t)(indices.size() / 3);
        if (n == 0) return;
        m_lo = m_hi = positions[indices[0]];
        for (uint32_t i : indices)
        {
            const float3 v = positions[i];
            m_lo = { std::min(m_lo.x, v.x), std::min(m_lo.y, v.y), std::min(m_lo.z, v.z) };
            m_hi = { std::max(m_hi.x, v.x), std::max(m_hi.y, v.y), std::max(m_hi.z, v.z) };
        }
        const float3 e = m_hi - m_lo;
        m_cell = std::max({ e.x, e.y, e.z, 1e-6f }) / 48;
        m_n[0] = (int)(e.x / m_cell) + 1;
        m_n[1] = (int)(e.y / m_cell) + 1;
        m_n[2] = (int)(e.z / m_cell) + 1;
        m_cells.resize((size_t)m_n[0] * m_n[1] * m_n[2]);
        for (uint32_t t = 0; t < n; ++t)
        {
            int lo[3] = { 1 << 30, 1 << 30, 1 << 30 }, hi[3] = { -1, -1, -1 };
            for (int c = 0; c < 3; ++c)
            {
                int k[3];
                cellOf(positions[indices[3 * t + c]], k);
                for (int a = 0; a < 3; ++a)
                {
                    lo[a] = std::min(lo[a], k[a]);
                    hi[a] = std::max(hi[a], k[a]);
                }
            }
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x) m_cells[((size_t)z * m_n[1] + y) * m_n[0] + x].push_back(t);
        }
    }

    // Closest point on the triangle list: its triangle and barycentrics.
    float3 closest(float3 p, uint32_t& triangle, float3& bary) const
    {
        int c[3];
        cellOf(p, c);
        float best = 1e30f;  // squared
        float3 point{};
        const int maxShell = std::max({ m_n[0], m_n[1], m_n[2] });
        for (int r = 0; r <= maxShell; ++r)
        {
            // Triangles not visited yet lie in cells r or more away from p's cell: at least (r - 1) cells from p.
            if (r > 1 && best < 1e29f && std::sqrt(best) <= (float)(r - 1) * m_cell) break;
            for (int z = c[2] - r; z <= c[2] + r; ++z)
                for (int y = c[1] - r; y <= c[1] + r; ++y)
                    for (int x = c[0] - r; x <= c[0] + r; ++x)
                    {
                        if (std::max({ std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2]) }) != r) continue;
                        if (x < 0 || y < 0 || z < 0 || x >= m_n[0] || y >= m_n[1] || z >= m_n[2]) continue;
                        for (uint32_t t : m_cells[((size_t)z * m_n[1] + y) * m_n[0] + x])
                        {
                            float3 b;
                            const float3 q = closestOnTriangle(p, m_positions[m_indices[3 * t]], m_positions[m_indices[3 * t + 1]], m_positions[m_indices[3 * t + 2]], b);
                            const float3 d = p - q;
                            if (dot(d, d) < best)
                            {
                                best = dot(d, d);
                                point = q;
                                triangle = t;
                                bary = b;
                            }
                        }
                    }
        }
        return point;
    }

    // Every triangle whose closest point to p lies within 'radius': its index, barycentrics and distance.
    template <typename Visit>
    void within(float3 p, float radius, Visit&& visit) const
    {
        int a[3], b[3];
        cellOf(p - float3{ radius, radius, radius }, a);
        cellOf(p + float3{ radius, radius, radius }, b);
        std::vector<uint32_t> seen;
        for (int z = a[2]; z <= b[2]; ++z)
            for (int y = a[1]; y <= b[1]; ++y)
                for (int x = a[0]; x <= b[0]; ++x)
                    for (uint32_t t : m_cells[((size_t)z * m_n[1] + y) * m_n[0] + x])
                    {
                        if (std::find(seen.begin(), seen.end(), t) != seen.end()) continue;
                        seen.push_back(t);
                        float3 bary;
                        const float3 q = closestOnTriangle(p, m_positions[m_indices[3 * t]], m_positions[m_indices[3 * t + 1]], m_positions[m_indices[3 * t + 2]], bary);
                        const float d = length(p - q);
                        if (d <= radius) visit(t, bary, d);
                    }
    }

private:
    void cellOf(float3 p, int k[3]) const
    {
        const float q[3] = { p.x - m_lo.x, p.y - m_lo.y, p.z - m_lo.z };
        for (int a = 0; a < 3; ++a) k[a] = std::clamp((int)std::floor(q[a] / m_cell), 0, m_n[a] - 1);
    }

    const std::vector<float3>& m_positions;
    std::span<const uint32_t> m_indices;
    float3 m_lo{}, m_hi{};
    float m_cell = 1;
    int m_n[3] = { 1, 1, 1 };
    std::vector<std::vector<uint32_t>> m_cells;
};

// Spectral norm of a 3 x 3 matrix (rows r0..r2): the square root of the largest eigenvalue of M^T M, in closed form
// (Smith 1961), with a relative margin of 1e-5 so rounding never makes it an underestimate.
float spectralNorm(const float m[3][3])
{
    double a[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[i][j] = (double)m[0][i] * m[0][j] + (double)m[1][i] * m[1][j] + (double)m[2][i] * m[2][j];
    const double p1 = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    const double q = (a[0][0] + a[1][1] + a[2][2]) / 3;
    double largest;
    if (p1 <= 1e-30)
        largest = std::max({ a[0][0], a[1][1], a[2][2] });
    else
    {
        const double p2 = (a[0][0] - q) * (a[0][0] - q) + (a[1][1] - q) * (a[1][1] - q) + (a[2][2] - q) * (a[2][2] - q) + 2 * p1;
        const double p = std::sqrt(p2 / 6);
        double b[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) b[i][j] = (a[i][j] - (i == j ? q : 0)) / p;
        const double det = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                           b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
        const double r = std::clamp(det / 2, -1.0, 1.0);
        largest = q + 2 * p * std::cos(std::acos(r) / 3);
    }
    return (float)(std::sqrt(std::max(largest, 0.0)) * (1 + 1e-5));
}

// A point as an affine combination of up to three mesh vertices.
struct Combination
{
    uint32_t vertex[3] = {};
    float weight[3] = {};
    uint32_t count = 0;
};

struct JointWeight
{
    uint32_t joint;
    float weight;
};

// A vertex's joint weights, normalised (linear blend skinning assumes they sum to 1).
uint32_t vertexWeights(const scene::Mesh& m, uint32_t v, JointWeight out[4])
{
    float sum = 0;
    for (int k = 0; k < 4; ++k) sum += m.skin.weights[4 * v + k];
    uint32_t n = 0;
    for (int k = 0; k < 4; ++k)
    {
        const float w = m.skin.weights[4 * v + k];
        if (w > 0 && sum > 0) out[n++] = { m.skin.joints[4 * v + k], w / sum };
    }
    return n;
}

// Blended weights of a combination (sparse, at most 12 joints).
uint32_t blended(const scene::Mesh& m, const Combination& c, JointWeight out[12])
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < c.count; ++k)
    {
        JointWeight w[4];
        const uint32_t wn = vertexWeights(m, c.vertex[k], w);
        for (uint32_t i = 0; i < wn; ++i)
        {
            uint32_t j = 0;
            while (j < n && out[j].joint != w[i].joint) ++j;
            if (j == n) out[n++] = { w[i].joint, 0 };
            out[j].weight += c.weight[k] * w[i].weight;
        }
    }
    return n;
}

float weightOf(const JointWeight* w, uint32_t n, uint32_t joint)
{
    for (uint32_t i = 0; i < n; ++i)
        if (w[i].joint == joint) return w[i].weight;
    return 0;
}

float3 pointOf(const scene::Mesh& m, const Combination& c)
{
    float3 p{};
    for (uint32_t k = 0; k < c.count; ++k) p = p + m.positions[c.vertex[k]] * c.weight[k];
    return p;
}

// One sampled pair: q (the cut side's point), its reference joint and per other joint (d_i, h_i).
struct PairRecord
{
    float3 q;
    uint32_t reference, first, count;  // entries
};
struct PairEntry
{
    uint32_t joint;
    float d, h;
};

// Adds one pair's record.
void addPair(const scene::Mesh& m, const ProxyPoseSkeleton& sk, const Combination& x, const Combination& q, ProxyPoseCoefficients& out,
             std::vector<PairRecord>& records, std::vector<PairEntry>& entries)
{
    const float3 px = pointOf(m, x), pq = pointOf(m, q);
    out.bindError = std::max(out.bindError, length(px - pq));
    JointWeight wx[12], wq[12];
    const uint32_t nx = blended(m, x, wx), nq = blended(m, q, wq);
    const uint32_t joints = (uint32_t)sk.centres.size();
    uint32_t r = joints;
    float best = -1;
    for (uint32_t i = 0; i < nq; ++i)
        if (wq[i].joint < joints && wq[i].weight > best)
        {
            best = wq[i].weight;
            r = wq[i].joint;
        }
    if (r == joints) return;
    uint32_t list[24];
    uint32_t nj = 0;
    for (uint32_t i = 0; i < nx; ++i) list[nj++] = wx[i].joint;
    for (uint32_t i = 0; i < nq; ++i)
        if (std::find(list, list + nj, wq[i].joint) == list + nj) list[nj++] = wq[i].joint;
    PairRecord rec{ pq, r, (uint32_t)entries.size(), 0 };
    for (uint32_t a = 0; a < nj; ++a)
    {
        const uint32_t i = list[a];
        if (i >= joints || i == r) continue;
        const float d = weightOf(wx, nx, i) - weightOf(wq, nq, i);
        float h = 0;
        for (const auto& [c, pc, wc, nc] : { std::tuple{ &x, px, wx, nx }, std::tuple{ &q, pq, wq, nq } })
            for (uint32_t k = 0; k < c->count; ++k)
            {
                JointWeight wv[4];
                const uint32_t nv = vertexWeights(m, c->vertex[k], wv);
                h += c->weight[k] * std::fabs(weightOf(wv, nv, i) - weightOf(wc, nc, i)) * length(m.positions[c->vertex[k]] - pc);
            }
        if (d == 0 && h == 0) continue;
        entries.push_back({ i, d, h });
        ++rec.count;
    }
    if (rec.count) records.push_back(rec);
}
} // namespace

ProxyPoseSkeleton proxyPoseSkeleton(const scene::Mesh& m)
{
    ProxyPoseSkeleton sk;
    const uint32_t joints = (uint32_t)m.skin.inverseBind.size();
    sk.centres.assign(joints, float3{});
    if (joints == 0 || m.skin.weights.size() < 4 * m.positions.size()) return sk;
    std::vector<double> total(joints, 0.0);
    std::vector<float3> sum(joints, float3{});
    for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v)
    {
        JointWeight w[4];
        const uint32_t n = vertexWeights(m, v, w);
        for (uint32_t i = 0; i < n; ++i)
            if (w[i].joint < joints)
            {
                total[w[i].joint] += w[i].weight;
                sum[w[i].joint] = sum[w[i].joint] + m.positions[v] * w[i].weight;
            }
    }
    for (uint32_t i = 0; i < joints; ++i)
        if (total[i] > 0) sk.centres[i] = sum[i] * (float)(1.0 / total[i]);
    return sk;
}

ProxyPoseCoefficients proxyPoseCoefficients(const scene::Mesh& m, ProxyPoseSkeleton& sk, std::span<const uint32_t> cut)
{
    ProxyPoseCoefficients out;
    const bool skinned = !sk.centres.empty() && m.skin.weights.size() >= 4 * m.positions.size();
    std::vector<PairRecord> records;
    std::vector<PairEntry> entries;
    if (cut.size() < 3 || m.indices.size() < 3) return out;
    const TriangleGrid cutGrid(m.positions, cut), sourceGrid(m.positions, std::span<const uint32_t>(m.indices));
    auto triangleCombination = [](std::span<const uint32_t> indices, uint32_t t, float3 bary) {
        Combination c;
        c.count = 3;
        for (int k = 0; k < 3; ++k) c.vertex[k] = indices[3 * t + k];
        c.weight[0] = bary.x;
        c.weight[1] = bary.y;
        c.weight[2] = bary.z;
        return c;
    };
    auto pair = [&](const Combination& x, const Combination& q) {
        if (skinned) addPair(m, sk, x, q, out, records, entries);
        else out.bindError = std::max(out.bindError, length(pointOf(m, x) - pointOf(m, q)));
    };
    // The correspondence: any point of the other surface gives a valid bound, so among the points within a tolerance of
    // the closest (1e-4 of the mesh extent) take the one whose joint weights differ least. Meshes duplicate vertices along
    // seams with different weights (a cap rim against the side); the nearest point there can be the wrong side of the seam.
    float3 lo = m.positions[0], hi = m.positions[0];
    for (const float3& v : m.positions)
    {
        lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
        hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
    }
    const float tolerance = 1e-4f * std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
    auto weightDistance = [&](const Combination& a, const Combination& b) {
        JointWeight wa[12], wb[12];
        const uint32_t na = blended(m, a, wa), nb = blended(m, b, wb);
        float d = 0;
        for (uint32_t i = 0; i < na; ++i) d += std::fabs(wa[i].weight - weightOf(wb, nb, wa[i].joint));
        for (uint32_t i = 0; i < nb; ++i)
            if (weightOf(wa, na, wb[i].joint) == 0) d += wb[i].weight;
        return d;
    };
    auto correspond = [&](const TriangleGrid& grid, std::span<const uint32_t> indices, const Combination& from) {
        const float3 p = pointOf(m, from);
        uint32_t t = 0;
        float3 bary;
        const float nearest = length(p - grid.closest(p, t, bary));
        Combination best = triangleCombination(indices, t, bary);
        if (!skinned) return best;
        float bestScore = weightDistance(from, best);
        grid.within(p, nearest + tolerance, [&](uint32_t ct, float3 cb, float) {
            const Combination c = triangleCombination(indices, ct, cb);
            const float score = weightDistance(from, c);
            if (score < bestScore)
            {
                bestScore = score;
                best = c;
            }
        });
        return best;
    };
    // Source vertices -> the cut (geometry the cut loses).
    for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v)
    {
        Combination x;
        x.count = 1;
        x.vertex[0] = v;
        x.weight[0] = 1;
        pair(x, correspond(cutGrid, cut, x));
    }
    // Cut corners, edge midpoints and centroids -> the source (geometry the cut adds).
    const float3 samples[7] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0.5f, 0.5f, 0 }, { 0, 0.5f, 0.5f }, { 0.5f, 0, 0.5f }, { 1 / 3.f, 1 / 3.f, 1 / 3.f } };
    for (uint32_t t = 0; t < (uint32_t)(cut.size() / 3); ++t)
        for (const float3& b : samples)
        {
            const Combination q = triangleCombination(cut, t, b);
            pair(correspond(sourceGrid, std::span<const uint32_t>(m.indices), q), q);
        }
    // Spatial clusters of the pairs by q (cells of 1/16 of the extent), centre = mean q; per cluster, per (joint,
    // reference) the maxima of |d| |q - centre| + h and |d|.
    const float cell = 16.0f / std::max(tolerance * 1e4f, 1e-6f);  // 1 / (extent / 16)
    std::unordered_map<uint64_t, uint32_t> clusterOf;
    std::vector<uint32_t> recordCluster(records.size());
    std::vector<float3> sum;
    std::vector<uint32_t> members;
    for (size_t k = 0; k < records.size(); ++k)
    {
        const float3 f = (records[k].q - lo) * cell;
        const uint64_t key = ((uint64_t)(uint32_t)std::max(0.0f, f.x) << 42) | ((uint64_t)(uint32_t)std::max(0.0f, f.y) << 21) | (uint64_t)(uint32_t)std::max(0.0f, f.z);
        const auto [it, added] = clusterOf.try_emplace(key, (uint32_t)sum.size());
        if (added)
        {
            sum.push_back(float3{});
            members.push_back(0);
        }
        recordCluster[k] = it->second;
        sum[it->second] = sum[it->second] + records[k].q;
        ++members[it->second];
    }
    std::vector<std::vector<ProxyPoseCoefficients::Term>> perCluster(sum.size());
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> perClusterKeys(sum.size());
    for (size_t k = 0; k < records.size(); ++k)
    {
        const uint32_t c = recordCluster[k];
        const float3 centre = sum[c] * (1.0f / members[c]);
        const PairRecord& rec = records[k];
        for (uint32_t e = rec.first; e < rec.first + rec.count; ++e)
        {
            const PairEntry& pe = entries[e];
            const std::pair<uint32_t, uint32_t> key{ pe.joint, rec.reference };
            auto& keys = perClusterKeys[c];
            size_t slot = std::find(keys.begin(), keys.end(), key) - keys.begin();
            if (slot == keys.size())
            {
                keys.push_back(key);
                perCluster[c].push_back({ 0, 0, 0 });
            }
            ProxyPoseCoefficients::Term& t = perCluster[c][slot];
            t.k1 = std::max(t.k1, std::fabs(pe.d) * length(rec.q - centre) + pe.h);
            t.k2 = std::max(t.k2, std::fabs(pe.d));
        }
    }
    // Register the (joint, reference) pairs in the skeleton (shared by every cut of the mesh).
    for (size_t c = 0; c < perCluster.size(); ++c)
    {
        ProxyPoseCoefficients::Cluster cl{ sum[c] * (1.0f / members[c]), (uint32_t)out.terms.size(), (uint32_t)perCluster[c].size() };
        for (size_t k = 0; k < perCluster[c].size(); ++k)
        {
            const auto it = std::find(sk.pairs.begin(), sk.pairs.end(), perClusterKeys[c][k]);
            perCluster[c][k].pair = (uint32_t)(it - sk.pairs.begin());
            if (it == sk.pairs.end()) sk.pairs.push_back(perClusterKeys[c][k]);
            out.terms.push_back(perCluster[c][k]);
        }
        out.clusters.push_back(cl);
    }
    return out;
}

void proxyPoseTerms(const ProxyPoseSkeleton& sk, std::span<const float4> palette, ProxyPoseTerms& out)
{
    const uint32_t joints = (uint32_t)std::min<size_t>(sk.centres.size(), palette.size() / 3);
    out.alpha.assign(sk.pairs.size(), 0.0f);
    out.palette = palette;
    out.s = 1;
    if (joints == 0) return;
    auto row = [&](uint32_t j, int r) { return palette[3 * j + r]; };
    auto apply = [&](uint32_t j, float3 p) {
        const float4 a = row(j, 0), b = row(j, 1), c = row(j, 2);
        return float3{ a.x * p.x + a.y * p.y + a.z * p.z + a.w, b.x * p.x + b.y * p.y + b.z * p.z + b.w, c.x * p.x + c.y * p.y + c.z * p.z + c.w };
    };
    // A bound on the spectral norm: min(Frobenius, sqrt(max column sum x max row sum)).
    auto norm2 = [](const float d[3][3]) {
        float frob = 0, rowMax = 0, col[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; ++k)
        {
            float rowSum = 0;
            for (int c = 0; c < 3; ++c)
            {
                frob += d[k][c] * d[k][c];
                rowSum += std::fabs(d[k][c]);
                col[c] += std::fabs(d[k][c]);
            }
            rowMax = std::max(rowMax, rowSum);
        }
        return std::min(std::sqrt(frob), std::sqrt(rowMax * std::max({ col[0], col[1], col[2] })));
    };
    float s = 0;
    for (uint32_t i = 0; i < joints; ++i)
    {
        float m[3][3];
        for (int k = 0; k < 3; ++k)
        {
            const float4 a = row(i, k);
            m[k][0] = a.x, m[k][1] = a.y, m[k][2] = a.z;
        }
        s = std::max(s, norm2(m));
    }
    out.s = s;
    for (size_t p = 0; p < sk.pairs.size(); ++p)
    {
        const auto [i, r] = sk.pairs[p];
        if (i >= joints || r >= joints) continue;
        float d[3][3];
        for (int k = 0; k < 3; ++k)
        {
            const float4 a = row(i, k), b = row(r, k);
            d[k][0] = a.x - b.x, d[k][1] = a.y - b.y, d[k][2] = a.z - b.z;
        }
        out.alpha[p] = norm2(d);
    }
}

float proxyPoseError(const ProxyPoseCoefficients& c, const ProxyPoseSkeleton& sk, const ProxyPoseTerms& t)
{
    // Every pair is in one cluster: the largest cluster sum bounds them all.
    const std::span<const float4> pal = t.palette;
    auto apply = [&](uint32_t j, float3 p) {
        const float4 a = pal[3 * j], b = pal[3 * j + 1], cc = pal[3 * j + 2];
        return float3{ a.x * p.x + a.y * p.y + a.z * p.z + a.w, b.x * p.x + b.y * p.y + b.z * p.z + b.w, cc.x * p.x + cc.y * p.y + cc.z * p.z + cc.w };
    };
    const size_t joints = pal.size() / 3;
    float worst = 0;
    for (const ProxyPoseCoefficients::Cluster& cl : c.clusters)
    {
        float sum = 0;
        for (uint32_t k = cl.first; k < cl.first + cl.count; ++k)
        {
            const ProxyPoseCoefficients::Term& term = c.terms[k];
            if (term.pair >= t.alpha.size()) continue;
            const auto [i, r] = sk.pairs[term.pair];
            if (i >= joints || r >= joints) continue;
            sum += t.alpha[term.pair] * term.k1 + length(apply(i, cl.centre) - apply(r, cl.centre)) * term.k2;
        }
        worst = std::max(worst, sum);
    }
    return t.s * c.bindError + worst;
}

std::vector<std::vector<std::vector<uint32_t>>> skinAwareCuts(const scene::Mesh& m, const ProxyPoseSkeleton& sk, uint32_t budget, float attributeWeight)
{
    std::vector<std::vector<std::vector<uint32_t>>> levels;
    const size_t vertices = m.positions.size();
    if (vertices == 0 || m.skin.weights.size() < 4 * vertices || sk.centres.empty()) return levels;
    float3 lo = m.positions[0], hi = m.positions[0];
    for (const float3& p : m.positions)
    {
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
    }
    // meshoptimizer measures positional error in units of the mesh extent: the attribute uses the same units.
    const float extent = std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-6f });
    std::vector<float> attributes(vertices * 3, 0.0f);
    for (uint32_t v = 0; v < (uint32_t)vertices; ++v)
    {
        JointWeight w[4];
        const uint32_t n = vertexWeights(m, v, w);
        float3 e{};
        for (uint32_t i = 0; i < n; ++i)
            if (w[i].joint < sk.centres.size()) e = e + sk.centres[w[i].joint] * w[i].weight;
        attributes[3 * v] = e.x / extent;
        attributes[3 * v + 1] = e.y / extent;
        attributes[3 * v + 2] = e.z / extent;
    }
    const float weights[3] = { attributeWeight, attributeWeight, attributeWeight };
    const size_t sourceTriangles = m.indices.size() / 3;
    auto cut = [&](double ratio) {
        std::vector<std::vector<uint32_t>> lists(m.submeshes.size());
        for (size_t s = 0; s < m.submeshes.size(); ++s)
        {
            const scene::Submesh& sub = m.submeshes[s];
            const uint32_t* indices = m.indices.data() + sub.indexOffset;
            if (ratio >= 1)
            {
                lists[s].assign(indices, indices + sub.indexCount);
                continue;
            }
            const size_t target = std::max<size_t>(3, (size_t)(sub.indexCount / 3 * ratio) * 3);
            lists[s].resize(sub.indexCount);
            float error = 0;
            lists[s].resize(meshopt_simplifyWithAttributes(lists[s].data(), indices, sub.indexCount, &m.positions[0].x, vertices, sizeof(float3), attributes.data(),
                                                           3 * sizeof(float), weights, 3, nullptr, target, 1.0f, meshopt_SimplifyLockBorder, &error));
        }
        return lists;
    };
    auto triangles = [](const std::vector<std::vector<uint32_t>>& lists) {
        size_t n = 0;
        for (const auto& l : lists) n += l.size() / 3;
        return n;
    };
    double ratio = sourceTriangles <= budget ? 1.0 : (double)budget / sourceTriangles;
    size_t previous = 0;
    for (int guard = 0; guard < 24; ++guard)
    {
        std::vector<std::vector<uint32_t>> lists = cut(ratio);
        const size_t n = triangles(lists);
        if (n == 0 || (previous && n * 10 > previous * 9)) break;  // no longer reducing
        levels.push_back(std::move(lists));
        previous = n;
        if (n <= 16) break;
        ratio *= 0.5;
    }
    return levels;
}
} // namespace unx::render::rt
