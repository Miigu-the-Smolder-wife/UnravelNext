#include "FeatureWidth.h"

#include "unx/core/Jobs.h"
#include "unx/core/Log.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <numeric>

namespace unx::clusterbuilder::detail
{
namespace
{
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr uint32_t kStack = 128;

uint32_t findRoot(std::vector<uint32_t>& parent, uint32_t x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

void unite(std::vector<uint32_t>& parent, uint32_t a, uint32_t b)
{
    a = findRoot(parent, a);
    b = findRoot(parent, b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);  // smaller root wins: deterministic
}

float3 minf(float3 a, float3 b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
float3 maxf(float3 a, float3 b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }
float axis(float3 v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

float boxDistanceSq(float3 p, float3 lo, float3 hi)
{
    float d = 0;
    for (int k = 0; k < 3; ++k)
    {
        const float v = axis(p, k), a = axis(lo, k), b = axis(hi, k);
        const float e = v < a ? a - v : (v > b ? v - b : 0.0f);
        d += e * e;
    }
    return d;
}

float segmentDistanceSq(float3 p, float3 a, float3 b)
{
    const float3 ab = b - a;
    const float len2 = dot(ab, ab);
    const float t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const float3 d = p - (a + ab * t);
    return dot(d, d);
}
} // namespace

void Bvh::build(const std::vector<float3>& lo, const std::vector<float3>& hi)
{
    const uint32_t count = (uint32_t)lo.size();
    order.resize(count);
    std::iota(order.begin(), order.end(), 0u);
    nodes.clear();
    if (!count) return;
    std::vector<float3> centre(count);
    for (uint32_t i = 0; i < count; ++i) centre[i] = (lo[i] + hi[i]) * 0.5f;
    struct Task
    {
        uint32_t node, first, count;
    };
    std::vector<Task> stack;
    nodes.push_back({});
    stack.push_back({ 0, 0, count });
    while (!stack.empty())
    {
        const Task task = stack.back();
        stack.pop_back();
        float3 bl{ kInf, kInf, kInf }, bh{ -kInf, -kInf, -kInf }, cl = bl, ch = bh;
        for (uint32_t i = task.first; i < task.first + task.count; ++i)
        {
            const uint32_t p = order[i];
            bl = minf(bl, lo[p]);
            bh = maxf(bh, hi[p]);
            cl = minf(cl, centre[p]);
            ch = maxf(ch, centre[p]);
        }
        nodes[task.node].lo = bl;
        nodes[task.node].hi = bh;
        if (task.count <= 4)
        {
            nodes[task.node].first = task.first;
            nodes[task.node].count = task.count;
            continue;
        }
        const float3 ext = ch - cl;
        const int k = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
        const uint32_t mid = task.first + task.count / 2;
        std::nth_element(order.begin() + task.first, order.begin() + mid, order.begin() + task.first + task.count, [&](uint32_t x, uint32_t y) {
            return axis(centre[x], k) < axis(centre[y], k) || (axis(centre[x], k) == axis(centre[y], k) && x < y);
        });
        const uint32_t left = (uint32_t)nodes.size();
        nodes.push_back({});
        nodes.push_back({});
        nodes[task.node].first = left;
        nodes[task.node].count = 0;
        stack.push_back({ left + 1, mid, task.first + task.count - mid });
        stack.push_back({ left, task.first, mid - task.first });
    }
}

MeshWidthContext::MeshWidthContext(const std::vector<float3>& positions, const std::vector<uint32_t>& indices)
    : m_positions(positions), m_indices(indices)
{
    const size_t n = positions.size();
    m_weld.resize(n);
    if (n) meshopt_generatePositionRemap(m_weld.data(), &positions[0].x, n, sizeof(float3));
    std::vector<uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    const uint32_t triangles = (uint32_t)(indices.size() / 3);
    for (uint32_t t = 0; t < triangles; ++t)
    {
        unite(parent, m_weld[indices[3 * t]], m_weld[indices[3 * t + 1]]);
        unite(parent, m_weld[indices[3 * t]], m_weld[indices[3 * t + 2]]);
    }
    m_component.resize(n);
    for (size_t v = 0; v < n; ++v) m_component[v] = findRoot(parent, (uint32_t)v);

    // Triangle BVH (thickness rays).
    std::vector<float3> lo(triangles), hi(triangles);
    float3 meshLo{ kInf, kInf, kInf }, meshHi{ -kInf, -kInf, -kInf };
    for (uint32_t t = 0; t < triangles; ++t)
    {
        const float3 a = positions[indices[3 * t]], b = positions[indices[3 * t + 1]], c = positions[indices[3 * t + 2]];
        lo[t] = minf(a, minf(b, c));
        hi[t] = maxf(a, maxf(b, c));
        meshLo = minf(meshLo, lo[t]);
        meshHi = maxf(meshHi, hi[t]);
    }
    m_epsilon = triangles ? 1e-5f * length(meshHi - meshLo) : 0.0f;
    m_triangles.build(lo, hi);

    // Boundary: welded edges used by exactly one triangle.
    std::vector<uint64_t> edges;
    edges.reserve(3 * (size_t)triangles);
    for (uint32_t t = 0; t < triangles; ++t)
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t a = m_weld[indices[3 * t + e]], b = m_weld[indices[3 * t + (e + 1) % 3]];
            if (a != b) edges.push_back((uint64_t)std::min(a, b) << 32 | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    std::vector<float3> slo, shi;
    for (size_t i = 0; i < edges.size();)
    {
        size_t j = i;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        if (j - i == 1)
        {
            const uint32_t a = (uint32_t)(edges[i] >> 32), b = (uint32_t)edges[i];
            m_segments.push_back(a);
            m_segments.push_back(b);
            slo.push_back(minf(positions[a], positions[b]));
            shi.push_back(maxf(positions[a], positions[b]));
        }
        i = j;
    }
    m_boundary.build(slo, shi);
    std::vector<float> triangleRadius;
    computeMedialRadii(triangleRadius);
    computeComponentWidths(triangleRadius);
}

// Component width: area-weighted median over its triangles of (thickness if the triangle has an opposite wall, else
// the diameter of the largest inscribed disk containing it).
void MeshWidthContext::computeComponentWidths(const std::vector<float>& R)
{
    const uint32_t triangles = (uint32_t)(m_indices.size() / 3);
    m_componentWidth.assign(m_positions.size(), FLT_MAX);
    std::vector<float> width(triangles);
    std::vector<double> area(triangles);
    Jobs::instance().parallelFor(triangles, [&](uint32_t t) {
        const float3 a = m_positions[m_indices[3 * t]], b = m_positions[m_indices[3 * t + 1]], c = m_positions[m_indices[3 * t + 2]];
        const float3 cr = cross(b - a, c - a);
        const float len = length(cr);
        area[t] = 0.5 * len;
        if (len <= 0)
        {
            width[t] = FLT_MAX;
            return;
        }
        const float3 n = cr / len;
        const float thick = thickness((a + b + c) / 3.0f - n * m_epsilon, -n, m_component[m_weld[m_indices[3 * t]]]);
        width[t] = thick < kInf ? thick : (R[t] < kInf ? 2 * R[t] : FLT_MAX);
    });
    std::vector<uint32_t> order(triangles);
    std::iota(order.begin(), order.end(), 0u);
    auto comp = [&](uint32_t t) { return m_component[m_weld[m_indices[3 * t]]]; };
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        const uint32_t cx = comp(x), cy = comp(y);
        if (cx != cy) return cx < cy;
        return width[x] != width[y] ? width[x] < width[y] : x < y;
    });
    for (size_t i = 0; i < order.size();)
    {
        size_t j = i;
        double total = 0;
        while (j < order.size() && comp(order[j]) == comp(order[i])) total += area[order[j++]];
        double acc = 0;
        for (size_t k = i; k < j; ++k)
        {
            acc += area[order[k]];
            if (acc >= 0.5 * total)
            {
                m_componentWidth[comp(order[i])] = width[order[k]];
                break;
            }
        }
        i = j;
    }
}

void MeshWidthContext::computeMedialRadii(std::vector<float>& R)
{
    const uint32_t triangles = (uint32_t)(m_indices.size() / 3);
    m_vertexRadius.assign(m_positions.size(), 0.0f);
    R.assign(triangles, -1.0f);
    if (!triangles) return;
    // Centred disks: r(t) at the triangle's sample farthest from the boundary of its component.
    std::vector<float> r(triangles);
    std::vector<float3> centre(triangles), centroid(triangles);
    std::vector<uint32_t> component(triangles);
    Jobs::instance().parallelFor(triangles, [&](uint32_t t) {
        const float3 a = m_positions[m_indices[3 * t]], b = m_positions[m_indices[3 * t + 1]], c = m_positions[m_indices[3 * t + 2]];
        const float3 samples[7] = { a, b, c, (a + b) * 0.5f, (b + c) * 0.5f, (c + a) * 0.5f, (a + b + c) / 3.0f };
        component[t] = m_component[m_weld[m_indices[3 * t]]];
        centroid[t] = samples[6];
        r[t] = -1;
        for (const float3& p : samples)
        {
            const float d = boundaryDistance(p, component[t]);
            if (d > r[t])
            {
                r[t] = d;
                centre[t] = p;
            }
        }
    });
    // Containing disks: largest first; each disk assigns its radius to every not yet assigned centroid of its
    // component inside it. A point BVH over centroids with per-node unassigned counts prunes finished regions.
    Bvh points;
    points.build(centroid, centroid);
    std::vector<uint32_t> open(points.nodes.size(), 0), parent(points.nodes.size(), UINT32_MAX);
    for (uint32_t n = (uint32_t)points.nodes.size(); n-- > 0;)
    {
        const Bvh::Node& node = points.nodes[n];
        if (node.count) open[n] = node.count;
        else
        {
            open[n] = open[node.first] + open[node.first + 1];
            parent[node.first] = parent[node.first + 1] = n;
        }
    }
    std::vector<uint32_t> leafOf(triangles);
    for (uint32_t n = 0; n < (uint32_t)points.nodes.size(); ++n)
        if (points.nodes[n].count)
            for (uint32_t i = points.nodes[n].first; i < points.nodes[n].first + points.nodes[n].count; ++i) leafOf[points.order[i]] = n;
    std::vector<uint32_t> byRadius(triangles);
    std::iota(byRadius.begin(), byRadius.end(), 0u);
    std::sort(byRadius.begin(), byRadius.end(), [&](uint32_t x, uint32_t y) { return r[x] != r[y] ? r[x] > r[y] : x < y; });
    std::vector<uint32_t> stack;
    for (uint32_t src : byRadius)
    {
        if (open[0] == 0) break;
        const float radius = r[src];
        if (!(radius < kInf))
        {
            if (R[src] < 0)  // no boundary in its component: unbounded sheet
            {
                R[src] = kInf;
                for (uint32_t n = leafOf[src]; n != UINT32_MAX; n = parent[n]) --open[n];
            }
            continue;
        }
        const float radius2 = radius * radius;
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            const uint32_t ni = stack.back();
            stack.pop_back();
            const Bvh::Node& n = points.nodes[ni];
            if (open[ni] == 0 || boxDistanceSq(centre[src], n.lo, n.hi) > radius2) continue;
            if (n.count == 0)
            {
                stack.push_back(n.first);
                stack.push_back(n.first + 1);
                continue;
            }
            for (uint32_t i = n.first; i < n.first + n.count; ++i)
            {
                const uint32_t t = points.order[i];
                if (R[t] >= 0 || component[t] != component[src]) continue;
                const float3 d = centroid[t] - centre[src];
                if (dot(d, d) > radius2) continue;
                R[t] = radius;
                for (uint32_t up = ni; up != UINT32_MAX; up = parent[up]) --open[up];
            }
        }
        if (R[src] < 0)  // its own disk misses its centroid (a sliver along the boundary): its own disk
        {
            R[src] = radius;
            for (uint32_t n = leafOf[src]; n != UINT32_MAX; n = parent[n]) --open[n];
        }
    }
    for (uint32_t t = 0; t < triangles; ++t)
        for (int k = 0; k < 3; ++k)
        {
            float& v = m_vertexRadius[m_weld[m_indices[3 * t + k]]];
            v = std::max(v, R[t]);
        }
}

// Distance along 'dir' to the nearest back-facing triangle (the inside of the opposite wall) of the same component.
float MeshWidthContext::thickness(float3 origin, float3 dir, uint32_t component) const
{
    if (m_triangles.nodes.empty()) return kInf;
    float best = kInf;
    auto safeInv = [](float d) { return 1.0f / (std::fabs(d) > 1e-30f ? d : (d < 0 ? -1e-30f : 1e-30f)); };  // no 0 * inf = NaN in the slab test
    const float3 inv{ safeInv(dir.x), safeInv(dir.y), safeInv(dir.z) };
    uint32_t stack[kStack];
    uint32_t top = 0;
    stack[top++] = 0;
    while (top)
    {
        const Bvh::Node& n = m_triangles.nodes[stack[--top]];
        float tmin = 0, tmax = best;
        for (int k = 0; k < 3; ++k)
        {
            float t0 = (axis(n.lo, k) - axis(origin, k)) * axis(inv, k), t1 = (axis(n.hi, k) - axis(origin, k)) * axis(inv, k);
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
        }
        if (!(tmin <= tmax)) continue;
        if (n.count == 0)
        {
            if (top + 2 > kStack) fail("MeshWidthContext: BVH deeper than the traversal stack");  // median splits: ~log2(n)
            stack[top++] = n.first + 1;
            stack[top++] = n.first;
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i)
        {
            const uint32_t t = m_triangles.order[i];
            const uint32_t ia = m_indices[3 * t], ib = m_indices[3 * t + 1], ic = m_indices[3 * t + 2];
            if (m_component[m_weld[ia]] != component) continue;
            const float3 a = m_positions[ia], b = m_positions[ib], c = m_positions[ic];
            const float3 e1 = b - a, e2 = c - a;
            if (dot(cross(e1, e2), dir) <= 0) continue;  // front face: not the inside of a wall
            const float3 p = cross(dir, e2);
            const float det = dot(e1, p);
            if (std::fabs(det) < 1e-30f) continue;
            const float invDet = 1.0f / det;
            const float3 s = origin - a;
            const float u = dot(s, p) * invDet;
            if (u < 0 || u > 1) continue;
            const float3 q = cross(s, e1);
            const float v = dot(dir, q) * invDet;
            if (v < 0 || u + v > 1) continue;
            const float d = dot(e2, q) * invDet;
            if (d > m_epsilon && d < best) best = d;
        }
    }
    return best;
}

// Euclidean distance to the nearest boundary edge of the same component (infinity when it has none).
float MeshWidthContext::boundaryDistance(float3 p, uint32_t component) const
{
    if (m_boundary.nodes.empty()) return kInf;
    float best = kInf;  // squared
    uint32_t stack[kStack];
    uint32_t top = 0;
    stack[top++] = 0;
    while (top)
    {
        const Bvh::Node& n = m_boundary.nodes[stack[--top]];
        if (boxDistanceSq(p, n.lo, n.hi) >= best) continue;
        if (n.count == 0)
        {
            if (top + 2 > kStack) fail("MeshWidthContext: BVH deeper than the traversal stack");
            // Nearer child last (popped first).
            const Bvh::Node& l = m_boundary.nodes[n.first];
            const Bvh::Node& r = m_boundary.nodes[n.first + 1];
            const bool leftNear = boxDistanceSq(p, l.lo, l.hi) <= boxDistanceSq(p, r.lo, r.hi);
            stack[top++] = leftNear ? n.first + 1 : n.first;
            stack[top++] = leftNear ? n.first : n.first + 1;
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i)
        {
            const uint32_t s = m_boundary.order[i];
            const uint32_t a = m_segments[2 * s], b = m_segments[2 * s + 1];
            if (m_component[a] != component) continue;
            best = std::min(best, segmentDistanceSq(p, m_positions[a], m_positions[b]));
        }
    }
    return best < kInf ? std::sqrt(best) : kInf;
}

MeshWidthContext::Width MeshWidthContext::clusterWidth(const uint32_t* indices, size_t indexCount) const
{
    const uint32_t triangles = (uint32_t)(indexCount / 3);
    // Pieces: triangles connected through shared welded edges.
    std::vector<uint32_t> parent(triangles);
    std::iota(parent.begin(), parent.end(), 0u);
    struct Edge
    {
        uint32_t a, b, triangle;
    };
    std::vector<Edge> edges;
    edges.reserve(3 * triangles);
    for (uint32_t t = 0; t < triangles; ++t)
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t a = m_weld[indices[3 * t + e]], b = m_weld[indices[3 * t + (e + 1) % 3]];
            edges.push_back({ std::min(a, b), std::max(a, b), t });
        }
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.a != y.a ? x.a < y.a : (x.b != y.b ? x.b < y.b : x.triangle < y.triangle); });
    for (size_t i = 1; i < edges.size(); ++i)
        if (edges[i].a == edges[i - 1].a && edges[i].b == edges[i - 1].b) unite(parent, edges[i].triangle, edges[i - 1].triangle);

    float bestWidth = kInf;
    bool bestFlat = false;
    float guard = FLT_MAX;
    std::vector<uint32_t> members;
    for (uint32_t root = 0; root < triangles; ++root)
    {
        if (findRoot(parent, root) != root) continue;
        members.clear();
        for (uint32_t t = root; t < triangles; ++t)
            if (findRoot(parent, t) == root) members.push_back(t);

        // Thickness from the piece's largest triangles (up to 8).
        std::vector<std::pair<float, uint32_t>> bySize;
        for (uint32_t t : members)
        {
            const float3 a = m_positions[indices[3 * t]], b = m_positions[indices[3 * t + 1]], c = m_positions[indices[3 * t + 2]];
            const float area2 = length(cross(b - a, c - a));
            if (area2 > 0) bySize.push_back({ area2, t });
        }
        if (bySize.empty()) continue;
        std::sort(bySize.begin(), bySize.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
        const uint32_t component = m_component[m_weld[indices[3 * bySize[0].second]]];
        guard = std::min(guard, m_componentWidth[component]);
        std::vector<float> depths;
        for (size_t i = 0; i < bySize.size() && i < 8; ++i)
        {
            const uint32_t t = bySize[i].second;
            const float3 a = m_positions[indices[3 * t]], b = m_positions[indices[3 * t + 1]], c = m_positions[indices[3 * t + 2]];
            const float3 n = cross(b - a, c - a) / bySize[i].first;
            depths.push_back(thickness((a + b + c) / 3.0f - n * m_epsilon, -n, component));
        }
        std::sort(depths.begin(), depths.end());
        const float thick = depths[depths.size() / 2];

        float width;
        bool flat;
        if (thick < kInf)
        {
            // A piece with an opposite wall is (part of) a solid: its silhouette is never narrower than that
            // thickness from any direction (tube -> diameter, slab -> thickness), even where the piece itself is a
            // narrow slice of the surface.
            width = thick;
            flat = false;
        }
        else
        {
            // Open sheet: the largest inscribed disk containing the piece's vertices.
            float radius = 0;
            for (uint32_t t : members)
                for (int k = 0; k < 3; ++k) radius = std::max(radius, m_vertexRadius[m_weld[indices[3 * t + k]]]);
            width = 2 * radius;
            flat = true;
        }
        if (width < bestWidth)
        {
            bestWidth = width;
            bestFlat = flat;
        }
    }
    Width out;
    // Closed or unbounded: no finite feature width.
    const float narrowest = bestWidth < kInf ? bestWidth : FLT_MAX;
    out.narrowest = bestFlat ? -narrowest : narrowest;
    out.guard = guard;
    return out;
}
} // namespace unx::clusterbuilder::detail
