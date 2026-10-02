// Mesh card generation (unx/scene/MeshCards.h). The steps and numbers follow Unreal's generator
// (MeshCardRepresentationUtilities.cpp, UE 5.8), lengths converted from cm to m:
//   1. voxel grid over the mesh bounds (10 cm voxels, at most 64 along the longest side; halved while the mesh gives
//      more than 10,000 surfels);
//   2. per direction and grid column, 32 rays through the whole mesh: a hit facing the direction (n.d >= 0.25) in a
//      cell that is not next to the previous hit's cell becomes a sample, with the first cell a card's near plane may
//      sit in to see it (minRayZ = the cell after the previous hit);
//   3. samples of a cell with the same minRayZ form one surfel; 25 hemisphere rays drop surfels inside geometry
//      (hits > 80 % and back-face hits > 20 %) and weight the rest (coverage x (open fraction + 1));
//   4. per direction: one cluster at near plane 0, then (unless the mesh is mostly two-sided) the best cluster over
//      all other near planes, repeated while one is valid (weighted coverage >= 15 cells, density > 0.2 / 3);
//   5. over all directions the least covered clusters are dropped until at most maxCards remain.
#include "unx/scene/MeshCards.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace unx::scene
{
namespace
{
constexpr float kTargetVoxelSize = 0.10f;       // TargetVoxelSize 10
constexpr float kBoundsGrow = 0.01f;            // + 1 on the extent, and the minimum extent
constexpr float kCardMarginZ = 0.10f;           // MarginZ 10
constexpr float kSurfaceRayBias = 0.001f;       // SurfaceRayBias 0.1
constexpr float kNormalThreshold = 0.25f;       // r.MeshCardRepresentation.NormalTreshold
constexpr float kMinDensity = 0.2f / 3.0f;      // r.MeshCardRepresentation.MinDensity / 3
constexpr float kMinClusterCoverage = 15.0f;
constexpr uint32_t kSurfelSamples = 32;         // rays per grid column
constexpr uint32_t kHemisphereDim = 5;          // trunc(sqrt(32)): 25 stratified directions
constexpr uint32_t kTargetSurfels = 10000;
constexpr uint32_t kNoPrimitive = 0xFFFFFFFFu;

float comp(float3 v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }
void setComp(float3& v, int a, float s) { (a == 0 ? v.x : (a == 1 ? v.y : v.z)) = s; }
float3 vmin(float3 a, float3 b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
float3 vmax(float3 a, float3 b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }

// ---- ray casting: a median-split BVH over the mesh triangles, closest hit, both faces --------------------------------
struct Hit
{
    float t = FLT_MAX;
    uint32_t primitive = kNoPrimitive;
};

class TriangleBvh
{
public:
    explicit TriangleBvh(const Mesh& mesh) : m_mesh(mesh)
    {
        const uint32_t count = (uint32_t)(mesh.indices.size() / 3);
        m_order.resize(count);
        m_centre.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            m_order[i] = i;
            m_centre[i] = (corner(i, 0) + corner(i, 1) + corner(i, 2)) / 3.0f;
        }
        m_nodes.reserve(count / 2 + 1);
        if (count != 0) build(0, count);
        m_centre.clear();
        m_centre.shrink_to_fit();
    }

    float3 normal(uint32_t primitive) const  // the front side's unit normal (counter-clockwise front faces)
    {
        const float3 n = cross(corner(primitive, 1) - corner(primitive, 0), corner(primitive, 2) - corner(primitive, 0));
        const float l = length(n);
        return l > 0 ? n / l : float3{ 0, 0, 0 };
    }

    // The nearest hit with t in (tNear, hit.t), ignoring 'skip'.
    Hit trace(float3 origin, float3 direction, float tNear, uint32_t skip) const
    {
        Hit hit;
        if (m_nodes.empty()) return hit;
        const float3 inv{ 1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z };
        struct Entry
        {
            uint32_t node;
            float t;
        };
        Entry stack[64];
        uint32_t top = 0;
        float enter = 0;
        if (!slab(m_nodes[0], origin, inv, tNear, hit.t, enter)) return hit;
        stack[top++] = { 0, enter };
        while (top != 0)
        {
            const Entry e = stack[--top];
            if (e.t > hit.t) continue;  // a hit found since is nearer than this box
            const Node& node = m_nodes[e.node];
            if (node.count != 0)
            {
                for (uint32_t i = 0; i < node.count; ++i)
                {
                    const uint32_t primitive = m_order[node.first + i];
                    if (primitive == skip) continue;
                    const float t = intersect(primitive, origin, direction);
                    if (t > tNear && t < hit.t)
                    {
                        hit.t = t;
                        hit.primitive = primitive;
                    }
                }
            }
            else if (top + 2 <= 64)
            {
                float tl = 0, tr = 0;
                const bool l = slab(m_nodes[node.first], origin, inv, tNear, hit.t, tl), r = slab(m_nodes[node.right], origin, inv, tNear, hit.t, tr);
                if (l && r)
                {
                    // the nearer child is popped first
                    if (tl <= tr)
                    {
                        stack[top++] = { node.right, tr };
                        stack[top++] = { node.first, tl };
                    }
                    else
                    {
                        stack[top++] = { node.first, tl };
                        stack[top++] = { node.right, tr };
                    }
                }
                else if (l)
                {
                    stack[top++] = { node.first, tl };
                }
                else if (r)
                {
                    stack[top++] = { node.right, tr };
                }
            }
        }
        return hit;
    }

    float3 corner(uint32_t primitive, uint32_t c) const { return m_mesh.positions[m_mesh.indices[primitive * 3 + c]]; }
    uint32_t triangleCount() const { return (uint32_t)(m_mesh.indices.size() / 3); }

    float intersect(uint32_t primitive, float3 o, float3 d) const  // Moller-Trumbore, both faces; < 0 = no hit
    {
        const float3 p0 = corner(primitive, 0);
        const float3 e1 = corner(primitive, 1) - p0, e2 = corner(primitive, 2) - p0;
        const float3 p = cross(d, e2);
        const float det = dot(e1, p);
        if (det == 0) return -1;
        const float invDet = 1.0f / det;
        const float3 s = o - p0;
        const float u = dot(s, p) * invDet;
        if (u < 0 || u > 1) return -1;
        const float3 q = cross(s, e1);
        const float v = dot(d, q) * invDet;
        if (v < 0 || u + v > 1) return -1;
        return dot(e2, q) * invDet;
    }

private:
    struct Node
    {
        float3 lo, hi;
        uint32_t first = 0;  // leaf: first entry of m_order; inner: the left child
        uint32_t count = 0;  // leaf: triangle count; inner: 0
        uint32_t right = 0;  // inner: the right child
    };

    static float halfArea(float3 lo, float3 hi)
    {
        const float3 d = hi - lo;
        return d.x * d.y + d.y * d.z + d.z * d.x;
    }

    static bool slab(const Node& n, float3 o, float3 inv, float tNear, float tFar, float& enter)
    {
        float t0 = tNear, t1 = tFar;
        for (int a = 0; a < 3; ++a)
        {
            float near = (comp(n.lo, a) - comp(o, a)) * comp(inv, a);
            float far = (comp(n.hi, a) - comp(o, a)) * comp(inv, a);
            if (near > far) std::swap(near, far);
            // NaN (0 x inf: the origin on a slab plane of an axis the ray does not move along) keeps the interval
            t0 = near > t0 ? near : t0;
            t1 = far < t1 ? far : t1;
        }
        enter = t0;
        return t0 <= t1;
    }

    uint32_t build(uint32_t first, uint32_t count)
    {
        const uint32_t index = (uint32_t)m_nodes.size();
        m_nodes.emplace_back();
        float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        float3 clo = lo, chi = hi;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t primitive = m_order[first + i];
            for (uint32_t c = 0; c < 3; ++c)
            {
                lo = vmin(lo, corner(primitive, c));
                hi = vmax(hi, corner(primitive, c));
            }
            clo = vmin(clo, m_centre[primitive]);
            chi = vmax(chi, m_centre[primitive]);
        }
        m_nodes[index].lo = lo;
        m_nodes[index].hi = hi;
        const float3 size = chi - clo;
        const int axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
        if (count <= 4 || comp(size, axis) <= 0)
        {
            m_nodes[index].first = first;
            m_nodes[index].count = count;
            return index;
        }
        uint32_t half = 0;
        {
            constexpr int kBins = 16;
            float bestCost = FLT_MAX;
            int bestAxis = -1, bestBin = 0;
            for (int a = 0; a < 3; ++a)
            {
                const float range = comp(size, a);
                if (range <= 0) continue;
                struct Bin
                {
                    float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    uint32_t count = 0;
                } bins[kBins];
                const float scale = (float)kBins / range;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t primitive = m_order[first + i];
                    Bin& b = bins[std::min(kBins - 1, (int)((comp(m_centre[primitive], a) - comp(clo, a)) * scale))];
                    ++b.count;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        b.lo = vmin(b.lo, corner(primitive, c));
                        b.hi = vmax(b.hi, corner(primitive, c));
                    }
                }
                float rightArea[kBins];
                uint32_t rightCount[kBins];
                {
                    float3 l{ FLT_MAX, FLT_MAX, FLT_MAX }, h{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    uint32_t n = 0;
                    for (int b = kBins - 1; b > 0; --b)
                    {
                        l = vmin(l, bins[b].lo);
                        h = vmax(h, bins[b].hi);
                        n += bins[b].count;
                        rightArea[b] = n != 0 ? halfArea(l, h) : 0.0f;
                        rightCount[b] = n;
                    }
                }
                float3 l{ FLT_MAX, FLT_MAX, FLT_MAX }, h{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                uint32_t n = 0;
                for (int b = 1; b < kBins; ++b)  // split before bin b
                {
                    l = vmin(l, bins[b - 1].lo);
                    h = vmax(h, bins[b - 1].hi);
                    n += bins[b - 1].count;
                    if (n == 0 || rightCount[b] == 0) continue;
                    const float cost = halfArea(l, h) * (float)n + rightArea[b] * (float)rightCount[b];
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        bestAxis = a;
                        bestBin = b;
                    }
                }
            }
            if (bestAxis >= 0)
            {
                const float scale = (float)kBins / comp(size, bestAxis), origin = comp(clo, bestAxis);
                const auto middle = std::partition(m_order.begin() + first, m_order.begin() + first + count, [&](uint32_t primitive)
                                                   { return std::min(kBins - 1, (int)((comp(m_centre[primitive], bestAxis) - origin) * scale)) < bestBin; });
                half = (uint32_t)(middle - (m_order.begin() + first));
            }
            if (half == 0 || half == count)
            {
                half = count / 2;
                std::nth_element(m_order.begin() + first, m_order.begin() + first + half, m_order.begin() + first + count,
                                 [&](uint32_t a, uint32_t b)
                                 {
                                     const float ca = comp(m_centre[a], axis), cb = comp(m_centre[b], axis);
                                     return ca != cb ? ca < cb : a < b;
                                 });
            }
        }
        const uint32_t left = build(first, half);
        const uint32_t right = build(first + half, count - half);
        m_nodes[index].first = left;
        m_nodes[index].right = right;
        return index;
    }

    const Mesh& m_mesh;
    std::vector<uint32_t> m_order;
    std::vector<float3> m_centre;
    std::vector<Node> m_nodes;
};

// ---- surfels ----------------------------------------------------------------------------------------------------------
struct Basis  // one direction's voxel grid: cell (x, y, z) -> mesh space
{
    float3 axisX, axisY, axisZ;  // axisZ = the ray direction (into the mesh, against the direction's normal)
    float3 offset;               // the grid's corner (cell 0, 0, 0 at the near plane)
    int size[3] = { 1, 1, 1 };
    float3 point(float x, float y, float z, float voxel) const { return offset + (axisX * x + axisY * y + axisZ * z) * voxel; }
};

struct Params
{
    float unit = 1;  // mesh units per metre (1 / metresPerUnit): the generator's lengths are metres
    float voxel = 0;
    float minOuterCoverage = 0;
    Basis basis[kMeshCardDirections];
};

struct Surfel
{
    int x, y, z;
    int minRayZ;            // a card's near plane must be at or after this cell to see the surfel
    float coverage;         // fraction of the column's rays that gave this surfel
    float weightedCoverage; // x (open fraction + 1)
};

struct Sample
{
    float3 position, normal;
    int minRayZ, cellZ;
};

float3 directionNormal(uint32_t direction)
{
    float3 n{ 0, 0, 0 };
    setComp(n, (int)(direction / 2), (direction & 1) != 0 ? 1.0f : -1.0f);
    return n;
}

Params initParams(float3 boundsMin, float3 boundsMax, float maxVoxels, float unit)
{
    Params p;
    p.unit = unit;
    const float targetVoxel = kTargetVoxelSize * unit;
    const float3 size = boundsMax - boundsMin;
    const float longest = std::max(size.x, std::max(size.y, size.z));
    const float sizeInVoxels = std::clamp(longest / targetVoxel + 0.5f, 1.0f, maxVoxels);
    p.voxel = std::max(targetVoxel, longest / sizeInVoxels);
    int n[3];
    for (int a = 0; a < 3; ++a) n[a] = (int)std::clamp(std::round(comp(size, a) / p.voxel), 1.0f, maxVoxels);
    const float3 centre = (boundsMin + boundsMax) * 0.5f;
    const float3 extent = float3{ (float)n[0], (float)n[1], (float)n[2] } * (p.voxel * 0.5f);
    const float3 lo = centre - extent, hi = centre + extent;
    for (uint32_t d = 0; d < kMeshCardDirections; ++d)
    {
        Basis& b = p.basis[d];
        float3 x, y, z;
        meshCardAxes(d, x, y, z);
        b.axisX = x;
        b.axisY = y;
        b.axisZ = -z;
        b.offset = lo;
        const int axis = (int)(d / 2);
        if ((d & 1) != 0) setComp(b.offset, axis, comp(hi, axis));
        const int ax = axis == 0 ? 1 : 0, ay = axis == 2 ? 1 : 2;
        b.size[0] = n[ax];
        b.size[1] = n[ay];
        b.size[2] = n[axis];
    }
    const float averageFace = 2.0f * (float)(n[0] * n[1] + n[0] * n[2] + n[1] * n[2]) / 6.0f;
    p.minOuterCoverage = std::min(kMinClusterCoverage, 0.5f * averageFace);
    return p;
}

uint32_t reverseBits(uint32_t v)
{
    v = (v >> 16) | (v << 16);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);
    return v;
}

float hash01(uint32_t v)  // deterministic jitter for the hemisphere strata
{
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; v ^= v >> 16;
    return (float)(v >> 8) * (1.0f / 16777216.0f);
}

std::vector<float3> hemisphereDirections()  // stratified, uniform over the hemisphere around +z
{
    std::vector<float3> out;
    for (uint32_t ix = 0; ix < kHemisphereDim; ++ix)
        for (uint32_t iy = 0; iy < kHemisphereDim; ++iy)
        {
            const uint32_t i = ix * kHemisphereDim + iy;
            const float u = ((float)ix + hash01(i * 2 + 1)) / (float)kHemisphereDim;
            const float v = ((float)iy + hash01(i * 2 + 2)) / (float)kHemisphereDim;
            const float phi = 6.28318530718f * u, cosTheta = v, sinTheta = std::sqrt(std::max(0.0f, 1.0f - v * v));
            out.push_back({ sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta });
        }
    return out;
}

void tangentBasis(float3 n, float3& t, float3& b)  // Frisvad
{
    if (n.z < -0.9999999f)
    {
        t = { 0, -1, 0 };
        b = { -1, 0, 0 };
        return;
    }
    const float a = 1.0f / (1.0f + n.z), c = -n.x * n.y * a;
    t = { 1.0f - n.x * n.x * a, c, -n.x };
    b = { c, 1.0f - n.y * n.y * a, -n.y };
}

struct Tracer
{
    const TriangleBvh& bvh;
    const std::vector<uint8_t>& twoSided;
    bool isTwoSided(uint32_t primitive) const { return primitive < twoSided.size() && twoSided[primitive] != 0; }
};

// Hemisphere rays from a surfel's samples: false when the surfel is inside geometry; 'open' = the fraction that left.
bool surfelVisibility(const Tracer& tr, const Sample* samples, uint32_t count, const std::vector<float3>& directions, float bias, float& open)
{
    uint32_t hits = 0, backHits = 0, misses = 0, s = 0;
    for (const float3& local : directions)
    {
        const Sample& sample = samples[s];
        float3 t, b;
        tangentBasis(sample.normal, t, b);
        const float3 direction = t * local.x + b * local.y + sample.normal * local.z;
        const Hit hit = tr.bvh.trace(sample.position, direction, bias, kNoPrimitive);
        if (hit.primitive != kNoPrimitive)
        {
            ++hits;
            if (dot(direction, tr.bvh.normal(hit.primitive)) > 0 && !tr.isTwoSided(hit.primitive)) ++backHits;
        }
        else
        {
            ++misses;
        }
        s = (s + 1) % count;
    }
    const float n = (float)directions.size();
    open = (float)misses / n;
    return !((float)hits > 0.8f * n && (float)backHits > 0.2f * n);
}

// The triangles whose footprint along a direction touches each grid column (the column rays are parallel: a ray can
// only hit triangles of its own column). start[c] .. start[c + 1] index 'triangles'.
struct ColumnLists
{
    std::vector<uint32_t> start, triangles;
};

ColumnLists columnLists(const TriangleBvh& bvh, const Basis& basis, float voxel)
{
    const int sx = basis.size[0], sy = basis.size[1];
    const uint32_t count = bvh.triangleCount();
    ColumnLists lists;
    lists.start.assign((size_t)sx * (size_t)sy + 1, 0);
    std::vector<int> rect((size_t)count * 4);
    for (uint32_t t = 0; t < count; ++t)
    {
        float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
        for (uint32_t c = 0; c < 3; ++c)
        {
            const float3 r = bvh.corner(t, c) - basis.offset;
            const float u = dot(r, basis.axisX) / voxel, v = dot(r, basis.axisY) / voxel;
            lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u);
            lo[1] = std::min(lo[1], v); hi[1] = std::max(hi[1], v);
        }
        int* q = &rect[(size_t)t * 4];
        q[0] = std::clamp((int)std::floor(lo[0]), 0, sx - 1);
        q[1] = std::clamp((int)std::floor(hi[0]), 0, sx - 1);
        q[2] = std::clamp((int)std::floor(lo[1]), 0, sy - 1);
        q[3] = std::clamp((int)std::floor(hi[1]), 0, sy - 1);
        if (hi[0] < 0 || lo[0] > (float)sx || hi[1] < 0 || lo[1] > (float)sy) { q[0] = 1; q[1] = 0; }  // outside the grid
        for (int y = q[2]; y <= q[3]; ++y)
            for (int x = q[0]; x <= q[1]; ++x) ++lists.start[(size_t)y * (size_t)sx + (size_t)x + 1];
    }
    for (size_t i = 1; i < lists.start.size(); ++i) lists.start[i] += lists.start[i - 1];
    lists.triangles.resize(lists.start.back());
    std::vector<uint32_t> fill(lists.start.begin(), lists.start.end() - 1);
    for (uint32_t t = 0; t < count; ++t)
    {
        const int* q = &rect[(size_t)t * 4];
        for (int y = q[2]; y <= q[3]; ++y)
            for (int x = q[0]; x <= q[1]; ++x) lists.triangles[fill[(size_t)y * (size_t)sx + (size_t)x]++] = t;
    }
    return lists;
}

double milliseconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

void generateSurfels(const Tracer& tr, const Params& params, uint32_t direction, const std::vector<float3>& hemisphere,
                     std::vector<Surfel>& surfels, MeshCardStats& stats)
{
    const auto start = std::chrono::steady_clock::now();
    double visibilityMs = 0;
    const Basis& basis = params.basis[direction];
    const float voxel = params.voxel;
    const float3 rayDirection = basis.axisZ;
    const float nearOffset = 2.0f * voxel;  // rays start outside the geometry
    const int sizeZ = basis.size[2];
    std::vector<Sample> samples;
    std::vector<uint32_t> cellCount((size_t)sizeZ), cellOffset((size_t)sizeZ);
    const ColumnLists lists = columnLists(tr.bvh, basis, voxel);
    std::vector<std::pair<float, uint32_t>> hits;  // one ray's hits (t, triangle), nearest first

    for (int cy = 0; cy < basis.size[1]; ++cy)
        for (int cx = 0; cx < basis.size[0]; ++cx)
        {
            samples.clear();
            for (uint32_t i = 0; i < kSurfelSamples; ++i)
            {
                const float jx = ((float)i + 0.5f) / (float)kSurfelSamples;
                const float jy = (float)((double)reverseBits(i) / 4294967296.0);
                const float3 origin = basis.point((float)cx + jx, (float)cy + jy, 0.0f, voxel) - rayDirection * nearOffset;
                hits.clear();
                const size_t column = (size_t)cy * (size_t)basis.size[0] + (size_t)cx;
                for (uint32_t k = lists.start[column]; k < lists.start[column + 1]; ++k)
                {
                    const float t = tr.bvh.intersect(lists.triangles[k], origin, rayDirection);
                    if (t > 0) hits.push_back({ t, lists.triangles[k] });
                }
                stats.columnTests += lists.start[column + 1] - lists.start[column];
                std::sort(hits.begin(), hits.end());
                size_t next = 0;
                int lastHitZ = -2;
                uint32_t skip = kNoPrimitive;
                float tNear = 0.0f;
                while (lastHitZ + 1 < sizeZ)
                {
                    // the nearest hit beyond tNear that is not the triangle just left
                    while (next < hits.size() && !(hits[next].first > tNear && hits[next].second != skip)) ++next;
                    if (next == hits.size()) break;
                    Hit hit;
                    hit.t = hits[next].first;
                    hit.primitive = hits[next].second;
                    const int hitZ = std::clamp((int)((hit.t - nearOffset) / voxel), 0, sizeZ - 1);
                    float3 normal = tr.bvh.normal(hit.primitive);
                    float nDotD = -dot(rayDirection, normal);
                    if (nDotD < 0 && tr.isTwoSided(hit.primitive))
                    {
                        nDotD = -nDotD;
                        normal = -normal;
                    }
                    if (nDotD >= kNormalThreshold && hitZ > lastHitZ + 1)
                    {
                        Sample s;
                        s.position = origin + rayDirection * hit.t;
                        s.normal = normal;
                        s.cellZ = hitZ;
                        s.minRayZ = lastHitZ >= 0 ? lastHitZ + 1 : 0;
                        samples.push_back(s);
                    }
                    lastHitZ = hitZ;
                    tNear = std::nextafter(std::max(nearOffset + (float)(lastHitZ + 1) * voxel, hit.t), std::numeric_limits<float>::infinity());
                    skip = hit.primitive;
                }
            }

            std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b)
                      {
                          if (a.cellZ != b.cellZ) return a.cellZ < b.cellZ;
                          if (a.minRayZ != b.minRayZ) return a.minRayZ > b.minRayZ;
                          if (a.position.x != b.position.x) return a.position.x < b.position.x;
                          if (a.position.y != b.position.y) return a.position.y < b.position.y;
                          return a.position.z < b.position.z;
                      });
            std::fill(cellCount.begin(), cellCount.end(), 0u);
            for (const Sample& s : samples) ++cellCount[(size_t)s.cellZ];
            cellOffset[0] = 0;
            for (int z = 1; z < sizeZ; ++z) cellOffset[(size_t)z] = cellOffset[(size_t)z - 1] + cellCount[(size_t)z - 1];

            for (int cz = 0; cz < sizeZ; ++cz)
            {
                const uint32_t count = cellCount[(size_t)cz], offset = cellOffset[(size_t)cz];
                uint32_t begin = 0;
                while (begin + 1 < count)  // a span of equal minRayZ gives one surfel (the source's loop: the cell's last lone sample gives none)
                {
                    uint32_t span = 0;
                    for (uint32_t i = begin; i < count && samples[offset + i].minRayZ == samples[offset + begin].minRayZ; ++i) ++span;
                    float open = 0;
                    const auto v0 = std::chrono::steady_clock::now();
                    const bool outside = surfelVisibility(tr, samples.data() + offset + begin, span, hemisphere, kSurfaceRayBias * params.unit, open);
                    visibilityMs += milliseconds(v0);
                    stats.hemisphereRays += hemisphere.size();
                    if (outside)
                    {
                        Surfel s;
                        s.x = cx;
                        s.y = cy;
                        s.z = cz;
                        s.minRayZ = samples[offset + begin].minRayZ;
                        s.coverage = (float)span / (float)kSurfelSamples;
                        s.weightedCoverage = s.coverage * (open + 1.0f);
                        surfels.push_back(s);
                    }
                    begin += span;
                }
            }
        }
    stats.visibilityMs += (float)visibilityMs;
    stats.columnMs += (float)(milliseconds(start) - visibilityMs);
}

// ---- clusters ---------------------------------------------------------------------------------------------------------
struct Cluster
{
    int lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX }, hi[3] = { -INT32_MAX, -INT32_MAX, -INT32_MAX };
    std::vector<uint32_t> surfels;
    int nearPlane = 0;
    float coverage = 0, weightedCoverage = 0;

    float density() const { return coverage / (float)((hi[0] + 1 - lo[0]) * (hi[1] + 1 - lo[1])); }
    bool valid(const Params& p) const
    {
        return !surfels.empty() && weightedCoverage >= (nearPlane == 0 ? p.minOuterCoverage : kMinClusterCoverage) && density() > kMinDensity;
    }
    float centre(int a) const { return (float)(lo[a] + hi[a]) * 0.5f; }
};

// Every unassigned surfel a card with this near plane sees.
void buildCluster(int nearPlane, const std::vector<Surfel>& surfels, const std::vector<uint8_t>& assigned, Cluster& c)
{
    c = Cluster{};
    c.nearPlane = nearPlane;
    for (uint32_t i = 0; i < surfels.size(); ++i)
    {
        const Surfel& s = surfels[i];
        if (assigned[i] != 0 || s.z < nearPlane || s.minRayZ > nearPlane) continue;
        c.surfels.push_back(i);
        const int coord[3] = { s.x, s.y, s.z };
        for (int a = 0; a < 3; ++a)
        {
            c.lo[a] = std::min(c.lo[a], coord[a]);
            c.hi[a] = std::max(c.hi[a], coord[a]);
        }
        c.coverage += s.coverage;
        c.weightedCoverage += s.weightedCoverage;
    }
}

void commit(std::vector<Cluster>& clusters, std::vector<uint8_t>& assigned, const Cluster& c)
{
    for (uint32_t i : c.surfels) assigned[i] = 1;
    clusters.push_back(c);
}
} // namespace

void meshCardAxes(uint32_t direction, float3& x, float3& y, float3& z)
{
    switch (direction / 2)
    {
    case 0: x = { 0, 1, 0 }; y = { 0, 0, 1 }; break;
    case 1: x = { 1, 0, 0 }; y = { 0, 0, 1 }; break;
    default: x = { 1, 0, 0 }; y = { 0, 1, 0 }; break;
    }
    z = directionNormal(direction);
}

MeshCards buildMeshCards(const Mesh& mesh, const std::vector<uint8_t>& triangleTwoSided, uint32_t maxCards, float metresPerUnit)
{
    MeshCards out;
    const float unit = 1.0f / (metresPerUnit > 0 ? metresPerUnit : 1.0f);
    const float grow = kBoundsGrow * unit, marginZ = kCardMarginZ * unit;
    const size_t triangles = mesh.indices.size() / 3;
    if (triangles == 0 || mesh.positions.empty() || maxCards == 0) return out;

    float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint32_t index : mesh.indices)
    {
        lo = vmin(lo, mesh.positions[index]);
        hi = vmax(hi, mesh.positions[index]);
    }
    {
        // not empty along any axis (a plane has no thickness): extent + 1 cm, at least 1 cm
        const float3 centre = (lo + hi) * 0.5f;
        float3 extent = (hi - lo) * 0.5f + float3{ grow, grow, grow };
        extent = vmax(extent, float3{ grow, grow, grow });
        out.boundsMin = centre - extent;
        out.boundsMax = centre + extent;
    }
    size_t twoSided = 0;
    for (size_t i = 0; i < triangles && i < triangleTwoSided.size(); ++i) twoSided += triangleTwoSided[i] != 0 ? 1 : 0;
    out.mostlyTwoSided = twoSided * 4 >= triangles;

    const auto buildStart = std::chrono::steady_clock::now();
    const TriangleBvh bvh(mesh);
    out.stats.bvhMs = (float)milliseconds(buildStart);
    const Tracer tracer{ bvh, triangleTwoSided };
    const std::vector<float3> hemisphere = hemisphereDirections();

    Params params;
    std::array<std::vector<Surfel>, kMeshCardDirections> surfels;
    float maxVoxels = 64;
    do
    {
        params = initParams(out.boundsMin, out.boundsMax, maxVoxels, unit);
        out.surfels = 0;
        for (uint32_t d = 0; d < kMeshCardDirections; ++d)
        {
            surfels[d].clear();
            generateSurfels(tracer, params, d, hemisphere, surfels[d], out.stats);
            out.surfels += (uint32_t)surfels[d].size();
        }
        maxVoxels *= 0.5f;
        ++out.stats.passes;
    } while (out.surfels > kTargetSurfels && maxVoxels > 1);

    const auto clusterStart = std::chrono::steady_clock::now();
    std::array<std::vector<Cluster>, kMeshCardDirections> clusters;
    for (uint32_t d = 0; d < kMeshCardDirections; ++d)
    {
        std::vector<uint8_t> assigned(surfels[d].size(), 0);
        Cluster temp;
        buildCluster(0, surfels[d], assigned, temp);
        if (temp.valid(params)) commit(clusters[d], assigned, temp);
        if (!out.mostlyTwoSided)
        {
            for (;;)
            {
                Cluster best;
                best.nearPlane = -1;
                for (int nearPlane = 1; nearPlane < params.basis[d].size[2]; ++nearPlane)
                {
                    buildCluster(nearPlane, surfels[d], assigned, temp);
                    if (temp.valid(params) && temp.weightedCoverage > best.weightedCoverage) best = temp;
                }
                if (!best.valid(params)) break;
                commit(clusters[d], assigned, best);
            }
        }
        std::sort(clusters[d].begin(), clusters[d].end(), [](const Cluster& a, const Cluster& b)
                  {
                      if (a.weightedCoverage != b.weightedCoverage) return a.weightedCoverage > b.weightedCoverage;
                      for (int axis = 0; axis < 3; ++axis)
                          if (a.centre(axis) != b.centre(axis)) return a.centre(axis) < b.centre(axis);
                      return false;
                  });
    }

    out.stats.clusterMs = (float)milliseconds(clusterStart);

    // at most maxCards over all directions: drop the least covered
    size_t total = 0;
    for (const auto& c : clusters) total += c.size();
    while (total > maxCards)
    {
        float smallest = FLT_MAX;
        uint32_t from = 0;
        for (uint32_t d = 0; d < kMeshCardDirections; ++d)
            if (!clusters[d].empty() && clusters[d].back().weightedCoverage < smallest)
            {
                smallest = clusters[d].back().weightedCoverage;
                from = d;
            }
        clusters[from].pop_back();
        --total;
    }

    for (uint32_t d = 0; d < kMeshCardDirections; ++d)
    {
        const Basis& basis = params.basis[d];
        // the mesh bounds in the grid's frame (metres from its corner along its axes)
        float3 localLo{ FLT_MAX, FLT_MAX, FLT_MAX }, localHi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int corner = 0; corner < 8; ++corner)
        {
            const float3 p{ (corner & 1) != 0 ? out.boundsMax.x : out.boundsMin.x, (corner & 2) != 0 ? out.boundsMax.y : out.boundsMin.y,
                            (corner & 4) != 0 ? out.boundsMax.z : out.boundsMin.z };
            const float3 r = p - basis.offset;
            const float3 l{ dot(r, basis.axisX), dot(r, basis.axisY), dot(r, basis.axisZ) };
            localLo = vmin(localLo, l);
            localHi = vmax(localHi, l);
        }
        for (const Cluster& c : clusters[d])
        {
            // the cluster's cells, with half a voxel before the near plane and one and a half behind the last cell
            float3 cLo = float3{ (float)c.lo[0], (float)c.lo[1], (float)c.lo[2] - 0.5f } * params.voxel;
            float3 cHi = float3{ (float)c.hi[0] + 1.0f, (float)c.hi[1] + 1.0f, (float)c.hi[2] + 1.5f } * params.voxel;
            cLo.x = std::max(cLo.x, localLo.x);
            cLo.y = std::max(cLo.y, localLo.y);
            cLo.z = std::max(cLo.z, localLo.z - marginZ);
            cHi.x = std::min(cHi.x, localHi.x);
            cHi.y = std::min(cHi.y, localHi.y);
            cHi.z = std::min(cHi.z, localHi.z + marginZ);
            const float3 centre = (cLo + cHi) * 0.5f;
            MeshCard card;
            card.origin = basis.offset + basis.axisX * centre.x + basis.axisY * centre.y + basis.axisZ * centre.z;
            card.extent = (cHi - cLo) * 0.5f;
            card.direction = d;
            out.cards.push_back(card);
        }
    }
    return out;
}

MeshCards buildMeshCards(const Mesh& mesh, const std::vector<Material>& materials, uint32_t maxCards, float metresPerUnit)
{
    std::vector<uint8_t> twoSided(mesh.indices.size() / 3, 0);
    for (const Submesh& sub : mesh.submeshes)
    {
        if (sub.material >= materials.size() || !materials[sub.material].twoSided) continue;
        for (uint32_t t = sub.indexOffset / 3; t < (sub.indexOffset + sub.indexCount) / 3 && t < twoSided.size(); ++t) twoSided[t] = 1;
    }
    return buildMeshCards(mesh, twoSided, maxCards, metresPerUnit);
}
} // namespace unx::scene
