#include "unx/clusterbuilder/ClusterBuilder.h"

#include "FeatureWidth.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/clusterbuilder/ClusterStream.h"
#include "unx/core/Jobs.h"
#include "unx/core/Sha256.h"
#include "unx/core/Log.h"
#include "ClusterBuilderSourceHash.generated.h"

#include <meshoptimizer.h>
#include <windows.h>

// meshoptimizer's cluster LOD scheme (demo/clusterlod.h, MIT, Arseny Kapoulkine): its helpers (clusterize, partition,
// lockBoundary, boundsCompute, mergeGroups) are used as is; the build loop below is ours because it adds the
// thin-feature error limit and drops the sloppy fallback (which merges disconnected leaves and blades into blobs); past
// that limit, thin geometry is thinned by whole pieces whose area the remaining ones take (Settings::thinPreserveArea).
#pragma warning(push, 0)
#pragma warning(disable : 4505)  // unused static helpers of the header
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace unx::clusterbuilder
{
namespace
{
constexpr uint32_t kNodeWidth = 8;       // children per hierarchy node (GPU traversal fan-out)
constexpr uint32_t kAttributeCount = 5;  // normal xyz, uv

struct ClusterOut
{
    std::vector<uint32_t> indices;  // mesh vertex indices
    int refined = -1;               // group this cluster was simplified from (-1 = source geometry)
    clodBounds lod{};               // sphere + error the cluster's own error is measured on
    uint32_t submesh = 0;
    float width = 0;                // signed minimum feature width
};

struct GroupOut
{
    clodBounds simplified{};  // parent sphere + error of this group's clusters (FLT_MAX: terminal)
    int depth = 0;
    uint32_t firstCluster = 0, clusterCount = 0;
};

struct MeshOut
{
    std::vector<ClusterOut> clusters;  // grouped: every group's clusters are contiguous
    std::vector<GroupOut> groups;
    std::vector<clodNode> nodes;
    uint32_t levelCount = 0;
    MeshStats stats;
    // The builder's own vertices (LodVertices): cluster vertex index (the mesh's vertex count) + i.
    std::vector<float3> lodPositions;
    std::vector<uint32_t> lodSources;
};

// The hierarchy's vertices: the mesh's own, then the ones made for enlarged thin geometry (enlargeIslands). Parallel
// arrays that every DAG of the mesh appends to; meshoptimizer reads them through clodMesh (bind after they grow).
struct Vertices
{
    std::vector<float3> positions;
    std::vector<float> attributes;            // kAttributeCount per vertex
    std::vector<unsigned char> boundaryLock;
    std::vector<unsigned int> remap;          // canonical vertex of the same position
    size_t sourceCount = 0;                   // the mesh's vertices
    std::vector<uint32_t> source;             // per added vertex: the mesh vertex whose attributes it has
    void bind(clodMesh& mesh) const
    {
        mesh.vertex_count = positions.size();
        mesh.vertex_positions = &positions[0].x;
        mesh.vertex_attributes = attributes.data();
        mesh.vertex_lock = boundaryLock.data();
    }
};

std::vector<unsigned int> simplifyLimited(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks,
                                          size_t targetCount, float maxError, float* error)
{
    if (targetCount > indices.size()) return indices;
    std::vector<unsigned int> lod(indices.size());
    const unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0u) |
                                 (config.simplify_regularize ? meshopt_SimplifyRegularize : 0u);
    lod.resize(meshopt_simplifyWithAttributes(lod.data(), indices.data(), indices.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
                                              mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count, locks.data(), targetCount,
                                              maxError, options, error));
    return lod;
}

// meshoptimizer rejects a collapse that turns a triangle by more than ~75 degrees relative to its state just before
// that collapse, so a chain of collapses can still end with a triangle facing away from the surface it replaces,
// typically a sliver between locked border vertices. Returns the triangles (offsets into lod) that face away from the
// source surface at any of their corners (area-weighted vertex normals of the group's input triangles).
std::vector<size_t> foldedTriangles(const clodMesh& mesh, const std::vector<unsigned int>& remap, const std::vector<unsigned int>& source,
                                    const std::vector<unsigned int>& lod)
{
    const size_t stride = mesh.vertex_positions_stride / sizeof(float);
    auto pos = [&](unsigned int v) { const float* p = mesh.vertex_positions + v * stride; return float3{ p[0], p[1], p[2] }; };
    std::unordered_map<unsigned int, float3> normals;  // by welded position
    normals.reserve(source.size());
    for (size_t t = 0; t < source.size(); t += 3)
    {
        const float3 n = cross(pos(source[t + 1]) - pos(source[t]), pos(source[t + 2]) - pos(source[t]));
        for (int k = 0; k < 3; ++k)
        {
            float3& acc = normals[remap[source[t + k]]];
            acc = acc + n;
        }
    }
    std::vector<size_t> folded;
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        const float3 n = cross(pos(lod[t + 1]) - pos(lod[t]), pos(lod[t + 2]) - pos(lod[t]));
        if (dot(n, n) <= 0) continue;
        // Every corner must agree: a sliver folded between border vertices on a slope can still point roughly along
        // the sum of the corner normals.
        if (dot(n, normals[remap[lod[t]]]) <= 0 || dot(n, normals[remap[lod[t + 1]]]) <= 0 || dot(n, normals[remap[lod[t + 2]]]) <= 0) folded.push_back(t);
    }
    return folded;
}

// ---- Thin geometry past the error limit (Settings::thinPreserveArea) ----------------------------------------------------
// A group of leaves, blades or needles cannot simplify within the thin-feature limit: a collapse that removes one
// would erode it. Such a group loses whole pieces instead (dropIslands) and the pieces that stay grow until the group
// has its area again (enlargeIslands), so every cut covers what the source covers.
constexpr double kIslandScaleMax = 4.0;  // largest enlargement of a piece along one axis
constexpr double kAreaTolerance = 0.02;  // a thinned group keeps its area within this, or stays terminal
constexpr double kDropShareMax = 0.75;   // of the candidate islands' triangles: the ones left take at most 4 x their area each

// Islands of a group: its input triangles connected through welded vertices. A free island has no locked vertex, so no
// other group, terminal cluster, submesh or instance uses its vertices: it can be dropped or moved as a whole without
// opening a border. width = the feature width of its component (FeatureWidth.h).
struct Islands
{
    std::unordered_map<unsigned int, uint32_t> ofVertex;  // welded vertex -> island
    std::vector<uint8_t> free;
    std::vector<float> width;
    uint32_t of(unsigned int welded) const { return ofVertex.find(welded)->second; }
};

template <typename WidthOf>
Islands islandsOf(const std::vector<unsigned int>& remap, const std::vector<unsigned char>& locks, const std::vector<unsigned int>& merged, const WidthOf& widthOf)
{
    Islands out;
    std::vector<uint32_t> parent;
    for (unsigned int v : merged)
        if (out.ofVertex.emplace(remap[v], (uint32_t)parent.size()).second) parent.push_back((uint32_t)parent.size());
    auto root = [&](uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t t = 0; t < merged.size(); t += 3)
        for (int k = 1; k < 3; ++k)
        {
            const uint32_t a = root(out.of(remap[merged[t]])), b = root(out.of(remap[merged[t + k]]));
            if (a != b) parent[std::max(a, b)] = std::min(a, b);  // smaller root wins: deterministic
        }
    // Numbered in the order the group's triangles reach them.
    std::vector<uint32_t> number(parent.size(), UINT32_MAX);
    uint32_t count = 0;
    for (unsigned int v : merged)
    {
        const uint32_t r = root(out.of(remap[v]));
        if (number[r] == UINT32_MAX) number[r] = count++;
    }
    for (auto& [vertex, island] : out.ofVertex) island = number[root(island)];
    out.free.assign(count, 1);
    out.width.assign(count, 0.0f);
    for (unsigned int v : merged)
    {
        const uint32_t island = out.of(remap[v]);
        if (locks[v] & meshopt_SimplifyVertex_Lock) out.free[island] = 0;
        out.width[island] = widthOf(v);
    }
    return out;
}

// Drops whole free islands from 'lod' until it has at most targetCount indices. The narrowest islands that hold twice
// the excess are the candidates, and of those every other one goes along a space-filling curve through their centres,
// so the group thins evenly instead of emptying one side. Returns whether any was dropped; widest = the largest width
// dropped: what a cut loses with them is at most that wide, and the group's error is at least it.
bool dropIslands(const Vertices& vertices, const Islands& islands, size_t targetCount, std::vector<unsigned int>& lod, float& widest)
{
    widest = 0;
    if (lod.size() <= targetCount) return false;
    struct Piece
    {
        uint32_t island;
        size_t indices;
    };
    std::vector<uint32_t> slot(islands.free.size(), UINT32_MAX);
    std::vector<Piece> pieces;
    std::vector<float3> centres;  // sum of the triangle centres
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        const uint32_t island = islands.of(vertices.remap[lod[t]]);
        if (!islands.free[island] || !(islands.width[island] < FLT_MAX)) continue;
        if (slot[island] == UINT32_MAX)
        {
            slot[island] = (uint32_t)pieces.size();
            pieces.push_back({ island, 0 });
            centres.push_back({});
        }
        pieces[slot[island]].indices += 3;
        centres[slot[island]] = centres[slot[island]] + (vertices.positions[lod[t]] + vertices.positions[lod[t + 1]] + vertices.positions[lod[t + 2]]) / 3.0f;
    }
    if (pieces.empty()) return false;
    const size_t excess = lod.size() - targetCount;
    std::vector<uint32_t> order(pieces.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        const float wx = islands.width[pieces[x].island], wy = islands.width[pieces[y].island];
        return wx != wy ? wx < wy : x < y;
    });
    size_t held = 0, candidates = 0;
    while (candidates < order.size() && held < 2 * excess) held += pieces[order[candidates++]].indices;
    order.resize(candidates);
    std::vector<float3> points(candidates);
    for (size_t i = 0; i < candidates; ++i) points[i] = centres[order[i]] / (float)(pieces[order[i]].indices / 3);
    std::vector<unsigned int> rank(candidates);
    meshopt_spatialSortRemap(rank.data(), &points[0].x, candidates, sizeof(float3));
    std::vector<uint32_t> along(candidates);
    for (size_t i = 0; i < candidates; ++i) along[rank[i]] = order[i];
    // Error diffusion along the curve: the dropped share of the indices met so far follows 'share'. Where the free
    // islands are too few to reach the target (the rest of the group is held by borders), some still stay to be enlarged.
    const double share = std::min(kDropShareMax, (double)excess / (double)held);
    double carried = 0;
    std::vector<uint8_t> dropped(islands.free.size(), 0);
    for (uint32_t i : along)
    {
        const Piece& piece = pieces[i];
        carried += share * (double)piece.indices;
        if (carried < 0.5 * (double)piece.indices) continue;
        carried -= (double)piece.indices;
        dropped[piece.island] = 1;
        widest = std::max(widest, islands.width[piece.island]);
    }
    size_t write = 0;
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        if (dropped[islands.of(vertices.remap[lod[t]])]) continue;
        for (int k = 0; k < 3; ++k) lod[write + k] = lod[t + k];
        write += 3;
    }
    const bool any = write < lod.size();
    lod.resize(write);
    return any;
}

// Unit eigenvectors (rows of 'axes') of a symmetric 3 x 3 matrix, by cyclic Jacobi rotations. 'a' is destroyed.
void symmetricAxes(double a[3][3], double axes[3][3])
{
    double v[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (!(off > 1e-14 * (std::fabs(a[0][0]) + std::fabs(a[1][1]) + std::fabs(a[2][2])))) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (a[p][q] == 0) continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), sn = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double kp = a[k][p], kq = a[k][q];
                    a[k][p] = c * kp - sn * kq;
                    a[k][q] = sn * kp + c * kq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double pk = a[p][k], qk = a[q][k];
                    a[p][k] = c * pk - sn * qk;
                    a[q][k] = sn * pk + c * qk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double kp = v[k][p], kq = v[k][q];
                    v[k][p] = c * kp - sn * kq;
                    v[k][q] = sn * kp + c * kq;
                }
            }
    }
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r) axes[k][r] = v[r][k];
}

// Gives a thinned group its area back (the reference's "preserve area"): every free island of 'lod' grows about its
// centroid by one offset d along its principal axes (factor 1 + 2 d / its extent along the axis, at most
// kIslandScaleMax). For a round island that is a uniform scale; a blade or a needle widens rather than lengthens, so
// the same area costs the smallest displacement. d is the one offset that brings the group to areaBefore; islands with
// a locked vertex stay where they are (other groups use their vertices), so the free ones take the whole deficit. The
// moved vertices are new vertices (the group's own clusters keep theirs at every finer cut) and 'lod' is rewritten to
// them; 'locks' gets their entries. Returns false when the area cannot be restored within kAreaTolerance (nothing is
// changed; the group stays terminal). moved = the largest displacement.
bool enlargeIslands(Vertices& vertices, std::vector<unsigned char>& locks, const Islands& islands, double areaBefore, std::vector<unsigned int>& lod, float& moved)
{
    struct Frame
    {
        double area = 0, centre[3] = {}, axes[3][3] = {}, extent[3] = {};
    };
    auto at = [&](unsigned int v, double out[3]) {
        const float3 p = vertices.positions[v];
        out[0] = p.x, out[1] = p.y, out[2] = p.z;
    };
    auto areaOf = [](const double e1[3], const double e2[3]) {
        const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        return 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    };
    std::vector<uint32_t> slot(islands.free.size(), UINT32_MAX);
    std::vector<Frame> frames;
    double lockedArea = 0, freeArea = 0;
    // Area and centroid (area-weighted triangle centres) of every free island.
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        double p[3][3];
        for (int k = 0; k < 3; ++k) at(lod[t + k], p[k]);
        const double e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] }, e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
        const double area = areaOf(e1, e2);
        const uint32_t island = islands.of(vertices.remap[lod[t]]);
        if (!islands.free[island])
        {
            lockedArea += area;
            continue;
        }
        if (slot[island] == UINT32_MAX)
        {
            slot[island] = (uint32_t)frames.size();
            frames.push_back({});
        }
        Frame& f = frames[slot[island]];
        f.area += area;
        for (int r = 0; r < 3; ++r) f.centre[r] += area * (p[0][r] + p[1][r] + p[2][r]) / 3;
        freeArea += area;
    }
    const double wanted = areaBefore - lockedArea;  // of the free islands
    moved = 0;
    if (freeArea >= wanted) return lockedArea + freeArea <= (1 + kAreaTolerance) * areaBefore;  // nothing was lost
    if (!(freeArea > 0)) return false;
    // Principal axes (second moments of the triangle corners about the centroid, area-weighted) and extents along them.
    for (Frame& f : frames)
        for (int r = 0; r < 3; ++r) f.centre[r] = f.area > 0 ? f.centre[r] / f.area : 0.0;
    std::vector<std::array<double, 9>> moments(frames.size());
    for (std::array<double, 9>& m : moments) m.fill(0.0);
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        const uint32_t island = islands.of(vertices.remap[lod[t]]);
        if (slot[island] == UINT32_MAX) continue;
        const Frame& f = frames[slot[island]];
        double p[3][3];
        for (int k = 0; k < 3; ++k) at(lod[t + k], p[k]);
        const double e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] }, e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
        const double area = areaOf(e1, e2);
        for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) moments[slot[island]][3 * r + c] += area * (p[k][r] - f.centre[r]) * (p[k][c] - f.centre[c]);
    }
    for (size_t i = 0; i < frames.size(); ++i)
    {
        double a[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) a[r][c] = moments[i][3 * r + c];
        symmetricAxes(a, frames[i].axes);
    }
    // Triangles of the free islands in their island's frame: corner coordinates along the axes.
    struct Local
    {
        uint32_t frame;
        double u[3][3];  // corner, axis
    };
    std::vector<Local> locals;
    std::vector<std::array<double, 6>> range(frames.size(), std::array<double, 6>{ DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX });
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        const uint32_t island = islands.of(vertices.remap[lod[t]]);
        if (slot[island] == UINT32_MAX) continue;
        const Frame& f = frames[slot[island]];
        Local l;
        l.frame = slot[island];
        for (int k = 0; k < 3; ++k)
        {
            double p[3];
            at(lod[t + k], p);
            for (int axis = 0; axis < 3; ++axis)
            {
                l.u[k][axis] = (p[0] - f.centre[0]) * f.axes[axis][0] + (p[1] - f.centre[1]) * f.axes[axis][1] + (p[2] - f.centre[2]) * f.axes[axis][2];
                range[l.frame][axis] = std::min(range[l.frame][axis], l.u[k][axis]);
                range[l.frame][3 + axis] = std::max(range[l.frame][3 + axis], l.u[k][axis]);
            }
        }
        locals.push_back(l);
    }
    double reach = 0;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        Frame& f = frames[i];
        for (int axis = 0; axis < 3; ++axis) f.extent[axis] = range[i][3 + axis] - range[i][axis];
        const double largest = std::max({ f.extent[0], f.extent[1], f.extent[2] });
        for (int axis = 0; axis < 3; ++axis)
            if (!(f.extent[axis] > 1e-4 * largest)) f.extent[axis] = 0;  // flat along this axis: nothing to scale
        reach = std::max(reach, largest);
    }
    auto factor = [](const Frame& f, int axis, double d) { return f.extent[axis] > 0 ? std::min(1 + 2 * d / f.extent[axis], kIslandScaleMax) : 1.0; };
    auto areaAt = [&](double d) {
        double total = 0;
        for (const Local& l : locals)
        {
            const Frame& f = frames[l.frame];
            double e1[3], e2[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                const double k = factor(f, axis, d);
                e1[axis] = k * (l.u[1][axis] - l.u[0][axis]);
                e2[axis] = k * (l.u[2][axis] - l.u[0][axis]);
            }
            total += areaOf(e1, e2);
        }
        return total;
    };
    // The offset: the area grows with it, until every axis of every island is at kIslandScaleMax.
    double lo = 0, hi = 0.5 * (kIslandScaleMax - 1) * reach;
    if (lockedArea + areaAt(hi) < (1 - kAreaTolerance) * areaBefore) return false;
    if (areaAt(hi) > wanted)
        for (int it = 0; it < 64; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (areaAt(mid) < wanted ? lo : hi) = mid;
        }
    const double offset = hi;

    // New vertices, one per moved vertex; vertices of one position stay at one position (remap).
    std::unordered_map<unsigned int, unsigned int> made, canonical;  // old vertex -> new; old welded vertex -> new canonical
    for (unsigned int& index : lod)
    {
        const unsigned int v = index, welded = vertices.remap[v];
        const uint32_t island = islands.of(welded);
        if (slot[island] == UINT32_MAX) continue;
        auto found = made.find(v);
        if (found == made.end())
        {
            const unsigned int fresh = (unsigned int)vertices.positions.size();
            const auto [first, isFirst] = canonical.emplace(welded, fresh);
            float3 position;
            if (isFirst)
            {
                const Frame& f = frames[slot[island]];
                double p[3], q[3] = { f.centre[0], f.centre[1], f.centre[2] };
                at(v, p);
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double u = (p[0] - f.centre[0]) * f.axes[axis][0] + (p[1] - f.centre[1]) * f.axes[axis][1] + (p[2] - f.centre[2]) * f.axes[axis][2];
                    for (int r = 0; r < 3; ++r) q[r] += factor(f, axis, offset) * u * f.axes[axis][r];
                }
                position = { (float)q[0], (float)q[1], (float)q[2] };
                moved = std::max(moved, length(position - vertices.positions[v]));
            }
            else position = vertices.positions[first->second];
            float attributes[kAttributeCount];
            std::memcpy(attributes, &vertices.attributes[(size_t)v * kAttributeCount], sizeof attributes);
            vertices.positions.push_back(position);
            vertices.attributes.insert(vertices.attributes.end(), attributes, attributes + kAttributeCount);
            vertices.boundaryLock.push_back(0);
            vertices.remap.push_back(first->second);
            vertices.source.push_back(v < vertices.sourceCount ? (uint32_t)v : vertices.source[v - vertices.sourceCount]);
            locks.push_back(locks[v] & meshopt_SimplifyVertex_Protect);
            found = made.emplace(v, fresh).first;
        }
        index = found->second;
    }
    return true;
}

// Feature widths of a level's enlarged geometry. The mesh's width context (FeatureWidth.h) measures the mesh's own
// vertices; enlarged islands are measured the same way on a context of their own: the triangles on made vertices of
// the level's clusters and of the terminal clusters before it (those stay drawn beside them). An island's vertices are
// all the mesh's or all made, so the two contexts never share a piece.
struct LevelWidths
{
    std::vector<float3> positions;                      // the level's made vertices
    std::vector<uint32_t> indices;                      // its triangles on them
    std::unordered_map<unsigned int, uint32_t> local;   // hierarchy vertex -> index in positions
    std::unique_ptr<detail::MeshWidthContext> context;  // over the two vectors above
    // Adds the triangles of a list that are on made vertices; returns them in local indices.
    std::vector<uint32_t> add(const Vertices& vertices, const std::vector<unsigned int>& triangles)
    {
        std::vector<uint32_t> added;
        for (size_t t = 0; t < triangles.size(); t += 3)
        {
            if (triangles[t] < vertices.sourceCount) continue;
            for (int k = 0; k < 3; ++k)
            {
                const unsigned int v = triangles[t + k];
                const auto [it, isNew] = local.emplace(v, (uint32_t)positions.size());
                if (isNew) positions.push_back(vertices.positions[v]);
                added.push_back(it->second);
            }
        }
        indices.insert(indices.end(), added.begin(), added.end());
        return added;
    }
};

void buildSubmesh(const Settings& settings, bool morphed, const clodConfig& config, clodMesh mesh, Vertices& vertices, std::vector<unsigned char>& locks,
                  const detail::MeshWidthContext& widths, uint32_t submesh, MeshOut& out)
{
    using namespace clod;
    const std::vector<unsigned int>& remap = vertices.remap;
    std::vector<Cluster> clusters = clusterize(config, mesh, mesh.indices, mesh.index_count);
    std::vector<detail::MeshWidthContext::Width> clusterWidth;
    for (Cluster& c : clusters)
    {
        c.bounds = boundsCompute(mesh, c.indices, 0.f);
        clusterWidth.push_back(widths.clusterWidth(c.indices.data(), c.indices.size()));
    }
    out.stats.sourceClusters += (uint32_t)clusters.size();
    std::vector<int> pending(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) pending[i] = (int)i;
    int depth = 0;
    // Positions of terminal clusters stay locked at every later level: those clusters are drawn at every coarser error,
    // so their neighbours' simplifications must keep the shared border (lockBoundary only locks borders between the
    // groups of the current level).
    std::vector<unsigned char> frozen(mesh.vertex_count, 0);  // by welded position
    // Enlarged thin geometry (thinPreserveArea): the widths of the pending clusters' made vertices, and the triangles
    // on made vertices of the terminal clusters so far.
    std::unique_ptr<LevelWidths> levelWidths;
    std::vector<unsigned int> terminalMade;
    auto componentWidth = [&](unsigned int v) {
        return v < vertices.sourceCount ? widths.componentWidth(v) : levelWidths->context->componentWidth(levelWidths->local.find(v)->second);
    };

    auto emit = [&](const std::vector<int>& group, const clodBounds& simplified) {
        GroupOut g;
        g.simplified = simplified;
        g.depth = depth;
        g.firstCluster = (uint32_t)out.clusters.size();
        g.clusterCount = (uint32_t)group.size();
        for (int ci : group)
        {
            ClusterOut c;
            c.indices.assign(clusters[ci].indices.begin(), clusters[ci].indices.end());
            c.refined = clusters[ci].refined;
            c.lod = clusters[ci].bounds;
            c.submesh = submesh;
            c.width = clusterWidth[ci].narrowest;
            if (simplified.error == FLT_MAX)
            {
                for (unsigned int v : c.indices) frozen[remap[v]] = 1;
                for (size_t t = 0; t < c.indices.size(); t += 3)
                    if (c.indices[t] >= vertices.sourceCount) terminalMade.insert(terminalMade.end(), c.indices.begin() + t, c.indices.begin() + t + 3);
            }
            out.clusters.push_back(std::move(c));
        }
        out.groups.push_back(g);
        if (simplified.error == FLT_MAX) ++out.stats.terminalGroups;
        return (int)out.groups.size() - 1;
    };

    // C4: a mesh with blend shapes or a vertex animation keeps its source clusters only (terminal, one group each): its
    // simplification errors would be measured on the bind pose, which the morphs leave (a coarse cut could miss the
    // curvature a shape adds). Exact at every distance; its cost is the source triangle count.
    if (morphed)
    {
        for (int ci : pending)
        {
            clodBounds bounds = clusters[ci].bounds;
            bounds.error = FLT_MAX;
            emit({ ci }, bounds);
        }
        return;
    }
    while (pending.size() > 1)
    {
        std::vector<std::vector<int>> groups = partition(config, mesh, clusters, pending, remap);
        pending.clear();
        lockBoundary(locks, groups, clusters, remap, mesh.vertex_lock);
        for (size_t v = 0; v < mesh.vertex_count; ++v)
            if (frozen[remap[v]]) locks[v] |= meshopt_SimplifyVertex_Lock;
        for (const std::vector<int>& group : groups)
        {
            std::vector<unsigned int> merged;
            float groupWidth = FLT_MAX;
            for (int ci : group)
            {
                merged.insert(merged.end(), clusters[ci].indices.begin(), clusters[ci].indices.end());
                groupWidth = std::min(groupWidth, clusterWidth[ci].guard);
            }
            const size_t target = size_t((merged.size() / 3) * config.simplify_ratio) * 3;
            clodBounds bounds = mergeGroups(clusters, group);  // encloses every member's sphere: monotone
            float error = 0.f;
            const float maxError = groupWidth >= FLT_MAX ? FLT_MAX : settings.maxRelativeWidthError * groupWidth;
            std::vector<unsigned int> simplified;
            bool stuck = false;
            // Thin geometry the error limit stops (thinPreserveArea): whole free islands are dropped instead, and the
            // ones that stay are enlarged below. What the limit protects is kept: no collapse erodes a thin feature.
            const bool thin = settings.thinPreserveArea && maxError < FLT_MAX;
            bool thinned = false;
            Islands islands;  // of the group; made when the limit first stops it
            // A fold-over is removed by locking the input vertices around its corners (the collapses that produced
            // it) and simplifying again at the same error; the rest of the group still simplifies.
            for (int attempt = 0;; ++attempt)
            {
                simplified = simplifyLimited(config, mesh, merged, locks, target, maxError, &error);
                stuck = simplified.size() > merged.size() * config.simplify_threshold;
                thinned = false;
                if (stuck && thin)
                {
                    if (islands.free.empty()) islands = islandsOf(remap, locks, merged, componentWidth);
                    float widest = 0;
                    thinned = dropIslands(vertices, islands, target, simplified, widest);
                    error = std::max(error, widest);
                    stuck = simplified.size() > merged.size() * config.simplify_threshold;
                }
                if (stuck) break;
                const std::vector<size_t> folded = foldedTriangles(mesh, remap, merged, simplified);
                if (folded.empty()) break;
                ++out.stats.foldRetries;
                if (attempt == 7)
                {
                    stuck = true;  // still folding after 8 attempts: keep the group's geometry
                    ++out.stats.foldTerminalGroups;
                    break;
                }
                std::unordered_map<unsigned int, bool> corner;
                for (size_t t : folded)
                    for (int k = 0; k < 3; ++k) corner[remap[simplified[t + k]]] = true;
                std::unordered_map<unsigned int, bool> ring;
                for (size_t t = 0; t < merged.size(); t += 3)
                    if (corner.count(remap[merged[t]]) || corner.count(remap[merged[t + 1]]) || corner.count(remap[merged[t + 2]]))
                        for (int k = 0; k < 3; ++k) ring[remap[merged[t + k]]] = true;
                for (unsigned int v : merged)
                    if (ring.count(remap[v])) locks[v] |= meshopt_SimplifyVertex_Lock;
            }
            if (!stuck && thinned)
            {
                double area = 0;
                for (size_t t = 0; t < merged.size(); t += 3)
                    area += 0.5 * length(cross(vertices.positions[merged[t + 1]] - vertices.positions[merged[t]], vertices.positions[merged[t + 2]] - vertices.positions[merged[t]]));
                const size_t firstMade = vertices.positions.size();
                float moved = 0;
                if (enlargeIslands(vertices, locks, islands, area, simplified, moved))
                {
                    vertices.bind(mesh);
                    frozen.resize(mesh.vertex_count, 0);
                    error += moved;  // the cut is within the simplification's error of the group, then moved by at most this
                    if (vertices.positions.size() > firstMade)
                    {
                        // The group's sphere (its new clusters' LOD sphere) also holds the enlarged islands.
                        const meshopt_Bounds reach = meshopt_computeSphereBounds(&vertices.positions[firstMade].x, vertices.positions.size() - firstMade, sizeof(float3), nullptr, 0);
                        clodBounds both[2] = { bounds, bounds };
                        std::memcpy(both[1].center, reach.center, sizeof reach.center);
                        both[1].radius = reach.radius;
                        bounds = boundsMerge(both, 2, sizeof(clodBounds));
                    }
                    ++out.stats.thinnedGroups;
                }
                else stuck = true;  // its area cannot be restored: the group keeps its geometry, as without thinPreserveArea
            }
            if (stuck)
            {
                bounds.error = FLT_MAX;  // terminal: stuck, the thin-feature error limit, or folds stop it
                emit(group, bounds);
                continue;
            }
            bounds.error = std::max(bounds.error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
            const int refined = emit(group, bounds);
            for (int ci : group) clusters[ci].indices = std::vector<unsigned int>();
            std::vector<Cluster> split = clusterize(config, mesh, simplified.data(), simplified.size());
            for (Cluster& c : split)
            {
                c.refined = refined;
                c.bounds = bounds;
                // (a cluster with made vertices gets its width when the level is complete, below)
                const bool made = std::any_of(c.indices.begin(), c.indices.end(), [&](unsigned int v) { return v >= vertices.sourceCount; });
                clusterWidth.push_back(made ? detail::MeshWidthContext::Width{} : widths.clusterWidth(c.indices.data(), c.indices.size()));
                clusters.push_back(std::move(c));
                pending.push_back((int)clusters.size() - 1);
            }
        }
        // Widths of the level's clusters with enlarged geometry, measured on that geometry (LevelWidths) as the source
        // clusters' are on the mesh: an enlarged leaf is wider, and so are its band distances and its next error limit.
        // Their triangles on the mesh's vertices keep the mesh's measure; the narrowest piece decides.
        if (vertices.positions.size() > vertices.sourceCount)
        {
            std::unique_ptr<LevelWidths> level;
            std::vector<std::pair<int, std::vector<uint32_t>>> made;  // cluster, its triangles on made vertices (level indices)
            for (int ci : pending)
            {
                const std::vector<unsigned int>& indices = clusters[ci].indices;
                if (std::none_of(indices.begin(), indices.end(), [&](unsigned int v) { return v >= vertices.sourceCount; })) continue;
                if (!level)
                {
                    level = std::make_unique<LevelWidths>();
                    level->add(vertices, terminalMade);
                }
                made.push_back({ ci, level->add(vertices, indices) });
            }
            if (level) level->context = std::make_unique<detail::MeshWidthContext>(level->positions, level->indices);
            for (const auto& [ci, local] : made)
            {
                detail::MeshWidthContext::Width width = level->context->clusterWidth(local.data(), local.size());
                std::vector<unsigned int> own;  // triangles on the mesh's vertices
                for (size_t t = 0; t < clusters[ci].indices.size(); t += 3)
                    if (clusters[ci].indices[t] < vertices.sourceCount) own.insert(own.end(), clusters[ci].indices.begin() + t, clusters[ci].indices.begin() + t + 3);
                if (!own.empty())
                {
                    const detail::MeshWidthContext::Width w = widths.clusterWidth(own.data(), own.size());
                    if (std::fabs(w.narrowest) < std::fabs(width.narrowest)) width.narrowest = w.narrowest;
                    width.guard = std::min(width.guard, w.guard);
                }
                clusterWidth[ci] = width;
            }
            levelWidths = std::move(level);
        }
        ++depth;
    }
    if (!pending.empty())
    {
        clodBounds bounds = clusters[pending[0]].bounds;
        bounds.error = FLT_MAX;
        emit(pending, bounds);
    }
}

// Sign-agnostic orientation of sheet triangles (a two-sided sheet's winding does not matter): the principal direction
// of sum(area n n^T) and cos of the largest angle between it and a triangle's normal line (0: 90 degrees or more).
struct SheetOrientation
{
    float3 axis{ 0, 0, 1 };
    float cosSpread = 0;
};

SheetOrientation sheetOrientation(const std::vector<float3>& positions, const uint32_t* indices, size_t indexCount)
{
    double s[3][3] = {};
    bool any = false;
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        float3 n = cross(positions[indices[i + 1]] - positions[indices[i]], positions[indices[i + 2]] - positions[indices[i]]);
        const float area2 = length(n);
        if (!(area2 > 0)) continue;
        n = n * (1.0f / area2);
        const double w = area2, v[3] = { n.x, n.y, n.z };
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] += w * v[r] * v[c];
        any = true;
    }
    SheetOrientation out;
    if (!any) return out;
    // Power iteration from the column of the largest diagonal (never orthogonal to the dominant eigenvector unless the
    // matrix is isotropic, where every axis is as good).
    int start = 0;
    for (int k = 1; k < 3; ++k)
        if (s[k][k] > s[start][start]) start = k;
    double v[3] = { s[0][start], s[1][start], s[2][start] };
    for (int it = 0; it < 64; ++it)
    {
        double u[3];
        for (int r = 0; r < 3; ++r) u[r] = s[r][0] * v[0] + s[r][1] * v[1] + s[r][2] * v[2];
        const double len = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(len > 0)) break;
        for (int r = 0; r < 3; ++r) v[r] = u[r] / len;
    }
    out.axis = normalize(float3{ (float)v[0], (float)v[1], (float)v[2] });
    float minDot = 1;
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        const float3 n = cross(positions[indices[i + 1]] - positions[indices[i]], positions[indices[i + 2]] - positions[indices[i]]);
        const float area2 = length(n);
        if (area2 > 0) minDot = std::min(minDot, std::fabs(dot(n, out.axis)) / area2);
    }
    out.cosSpread = std::max(minDot, 0.0f);
    return out;
}

// Orientation class of a planar component (spread within 5 degrees): one of 144 cells of an octahedral map of the
// axis hemisphere; 0 for curved or solid components. Components share no vertices, so building each class as its own
// DAG needs no locks, and a cluster never mixes sheets of different orientations (crossed cards): its sheet spread stays
// small, so only the sheets seen edge-on go to band B.
int32_t orientationClass(const SheetOrientation& o)
{
    constexpr float kPlanarCos = 0.9961947f;  // cos 5 degrees
    constexpr int32_t kCells = 12;             // cells of about 8 degrees
    if (o.cosSpread < kPlanarCos) return 0;
    float3 a = o.axis;
    if (a.z < 0 || (a.z == 0 && (a.y < 0 || (a.y == 0 && a.x < 0)))) a = a * -1.0f;
    const float l1 = std::fabs(a.x) + std::fabs(a.y) + a.z;
    const float px = a.x / l1, py = a.y / l1;  // |px| + |py| <= 1
    const float u = std::clamp((px + py + 1) * 0.5f, 0.0f, 0.999999f), w = std::clamp((py - px + 1) * 0.5f, 0.0f, 0.999999f);
    return 1 + (int32_t)(u * kCells) + kCells * (int32_t)(w * kCells);
}

MeshOut buildMesh(const scene::Mesh& m, const Settings& settings, const std::vector<uint32_t>& seamVertices)
{
    const auto t0 = std::chrono::steady_clock::now();
    MeshOut out;
    const size_t vertexCount = m.positions.size();
    out.stats.sourceTriangles = (uint32_t)(m.indices.size() / 3);
    if (m.submeshes.size() > 0xFFFF) fail("cluster builder: mesh '%s' has %zu submeshes (limit 65536)", m.name.c_str(), m.submeshes.size());
    if (vertexCount == 0 || m.indices.empty()) return out;

    detail::MeshWidthContext widths(m.positions, m.indices);
    const std::vector<uint32_t>& remap = widths.weld();

    // Attributes for attribute-aware simplification: normal (weighted) and uv (protected at seams only).
    std::vector<float> attributes(vertexCount * kAttributeCount, 0.0f);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        if (v < m.normals.size())
        {
            attributes[v * kAttributeCount + 0] = m.normals[v].x;
            attributes[v * kAttributeCount + 1] = m.normals[v].y;
            attributes[v * kAttributeCount + 2] = m.normals[v].z;
        }
        if (v < m.uv0.size())
        {
            attributes[v * kAttributeCount + 3] = m.uv0[v].x;
            attributes[v * kAttributeCount + 4] = m.uv0[v].y;
        }
    }
    const float weights[kAttributeCount] = { 0.5f, 0.5f, 0.5f, 0.0f, 0.0f };

    // Submesh boundaries stay fixed (clusters never span submeshes, so each submesh's DAG must keep the shared edge).
    std::vector<uint32_t> owner(vertexCount, UINT32_MAX);
    std::vector<unsigned char> boundaryLock(vertexCount, 0);
    for (uint32_t s = 0; s < (uint32_t)m.submeshes.size(); ++s)
    {
        const scene::Submesh& sm = m.submeshes[s];
        for (uint32_t i = sm.indexOffset; i < sm.indexOffset + sm.indexCount; ++i)
        {
            const uint32_t w = remap[m.indices[i]];
            if (owner[w] == UINT32_MAX) owner[w] = s;
            else if (owner[w] != s) boundaryLock[w] = meshopt_SimplifyVertex_Lock;
        }
    }
    // Seams with other instances (findSeams): the shared border stays at the source vertices at every level, so two
    // meshes that meet (terrain tiles, modular pieces) are watertight whatever cut each one draws.
    for (uint32_t v : seamVertices) boundaryLock[remap[v]] = meshopt_SimplifyVertex_Lock;
    for (size_t v = 0; v < vertexCount; ++v) boundaryLock[v] = boundaryLock[remap[v]];

    // The hierarchy's vertices: the mesh's; enlarged thin geometry adds its own (enlargeIslands).
    Vertices vertices;
    vertices.positions = m.positions;
    vertices.attributes = std::move(attributes);
    vertices.boundaryLock = std::move(boundaryLock);
    vertices.remap = remap;
    vertices.sourceCount = vertexCount;

    clodConfig config = clodDefaultConfig(settings.clusterTriangles);
    config.max_vertices = settings.clusterVertices;
    // Disconnected geometry (a leaf, a blade per component): the flex builder ends a meshlet at min_triangles when the
    // best next triangle is not connected, so min_triangles sets the fill of foliage clusters (v1.36).
    config.min_triangles = settings.clusterMinTriangles;
#if MESHOPTIMIZER_VERSION < 1000
    config.min_triangles &= ~3;
#endif
    config.simplify_fallback_sloppy = false;   // never merge disconnected geometry into blobs
    config.simplify_error_edge_limit = 0.0f;   // error is geometric only
    config.optimize_bounds = false;            // cluster.bounds = the LOD sphere; tight culling bounds computed below

    // Each submesh is built as one DAG per width class: connected components whose widths differ by 4x or more never
    // share a group, so a group's error limit (thin-feature guard) suits every component in it. Components share no
    // vertices, so splitting them needs no locks.
    auto widthClass = [&](uint32_t vertex) {
        const float w = widths.componentWidth(vertex);
        return w >= FLT_MAX ? INT32_MAX : (int32_t)std::floor(std::log(std::max(w, 1e-9f)) / std::log(4.0f));
    };
    // Orientation class per connected component (orientationClass): planar components are split by orientation, so a
    // cluster's sheet spread stays small and only the sheets seen edge-on leave band A. Components narrower than
    // sheetOrientationMinWidth (grass blades: band B beyond a few metres whatever their orientation) are clustered
    // across orientations: one class per blade would leave a blade per cluster (v1.36).
    std::unordered_map<uint32_t, std::vector<uint32_t>> componentTriangles;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        std::vector<uint32_t>& t = componentTriangles[widths.component(m.indices[i])];
        t.insert(t.end(), { m.indices[i], m.indices[i + 1], m.indices[i + 2] });
    }
    std::unordered_map<uint32_t, int32_t> componentOrientation;
    for (const auto& [component, tris] : componentTriangles)
        componentOrientation[component] =
            widths.componentWidth(tris[0]) < settings.sheetOrientationMinWidth ? 0 : orientationClass(sheetOrientation(m.positions, tris.data(), tris.size()));
    componentTriangles.clear();
    for (uint32_t s = 0; s < (uint32_t)m.submeshes.size(); ++s)
    {
        const scene::Submesh& sm = m.submeshes[s];
        // Degenerate triangles (repeated welded vertex or zero area) cover nothing and are dropped.
        std::map<std::pair<int32_t, int32_t>, std::vector<unsigned int>> byClass;  // (width class, orientation class)
        for (uint32_t i = sm.indexOffset; i + 2 < sm.indexOffset + sm.indexCount; i += 3)
        {
            const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            if (remap[a] == remap[b] || remap[b] == remap[c] || remap[a] == remap[c]) continue;
            if (length(cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a])) <= 0) continue;
            std::vector<unsigned int>& target = byClass[{ widthClass(a), componentOrientation[widths.component(a)] }];
            target.insert(target.end(), { a, b, c });
        }
        for (const auto& [cls, indices] : byClass)
        {
        clodMesh mesh{};
        mesh.indices = indices.data();
        mesh.index_count = indices.size();
        vertices.bind(mesh);  // vertex count, positions, attributes, locks
        mesh.vertex_positions_stride = sizeof(float3);
        mesh.vertex_attributes_stride = kAttributeCount * sizeof(float);
        mesh.attribute_weights = weights;
        mesh.attribute_count = kAttributeCount;
        mesh.attribute_protect_mask = (1u << kAttributeCount) - 1;  // normal and uv seams

        // Protect attribute discontinuities (same position, different attributes), as clodBuild does.
        std::vector<unsigned char> locks(mesh.vertex_count, 0);
        for (size_t v = 0; v < mesh.vertex_count; ++v)
        {
            const uint32_t r = vertices.remap[v];
            if (r == v) continue;
            for (uint32_t k = 0; k < kAttributeCount; ++k)
                if (vertices.attributes[v * kAttributeCount + k] != vertices.attributes[r * kAttributeCount + k])
                {
                    locks[v] |= meshopt_SimplifyVertex_Protect;
                    break;
                }
        }
        buildSubmesh(settings, settings.noSimplification || !m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0, config, mesh, vertices, locks, widths, s, out);
        }
    }
    out.lodPositions.assign(vertices.positions.begin() + vertexCount, vertices.positions.end());
    // Compressed cluster vertices store grid coordinates (ClusterStream.h): the builder's own vertices go on the mesh's
    // grid, as the cook puts the mesh's (snapPositions).
    if (settings.compression && streamMesh(m))
    {
        const float step = positionStep(m.positions, settings);
        for (float3& p : out.lodPositions) p = { (float)gridCoordinate(p.x, step) * step, (float)gridCoordinate(p.y, step) * step, (float)gridCoordinate(p.z, step) * step };
    }
    out.lodSources = std::move(vertices.source);
    out.stats.lodVertices = (uint32_t)out.lodPositions.size();

    // DAG invariants the cut relies on (exactly one of a group and its simplification is drawn at any error):
    // a cluster's own error is its source group's error, and never exceeds the error of the group it is in.
    for (const GroupOut& g : out.groups)
        for (uint32_t k = 0; k < g.clusterCount; ++k)
        {
            const ClusterOut& c = out.clusters[g.firstCluster + k];
            const float own = c.refined < 0 ? 0.0f : out.groups[c.refined].simplified.error;
            if (c.refined >= 0 && c.lod.error != own) fail("cluster builder: '%s' cluster error %g != source group error %g", m.name.c_str(), c.lod.error, own);
            if (!(own <= g.simplified.error)) fail("cluster builder: '%s' non-monotone errors: cluster %g in group %g (depth %d)", m.name.c_str(), own, g.simplified.error, g.depth);
        }

    // Hierarchy: one tree per DAG depth over all groups of the mesh (roots are nodes 0 .. levelCount-1).
    int maxDepth = -1;
    for (const GroupOut& g : out.groups) maxDepth = std::max(maxDepth, g.depth);
    out.levelCount = (uint32_t)(maxDepth + 1);
    std::vector<clodGroup> groups(out.groups.size());
    for (size_t i = 0; i < groups.size(); ++i) groups[i] = { out.groups[i].depth, out.groups[i].simplified };
    if (!groups.empty())
    {
        out.nodes.resize(clodBuildHierarchyBound(groups.size(), kNodeWidth, out.levelCount));
        out.nodes.resize(clodBuildHierarchy(out.nodes.data(), groups.data(), groups.size(), kNodeWidth, out.levelCount));
    }
    out.stats.clusters = (uint32_t)out.clusters.size();
    out.stats.groups = (uint32_t)out.groups.size();
    out.stats.depth = out.levelCount;
    out.stats.nodes = (uint32_t)out.nodes.size();
    out.stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

void appendNamed(render::ClusterData& data, const char* name, const void* bytes, size_t size, uint32_t stride)
{
    for (render::ClusterData::Named& n : data.named)
        if (n.name == name)
        {
            const uint8_t* p = static_cast<const uint8_t*>(bytes);
            n.bytes.insert(n.bytes.end(), p, p + size);
            return;
        }
    render::ClusterData::Named n;
    n.name = name;
    n.stride = stride;
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    n.bytes.assign(p, p + size);
    data.named.push_back(std::move(n));
}
} // namespace

Settings Settings::fromQuality(const QualityConfig& q)
{
    Settings s;
    s.clusterTriangles = (uint32_t)q.integer("visibility.cluster_triangles");
    s.clusterVertices = (uint32_t)q.integer("visibility.cluster_vertices");
    s.maxRelativeWidthError = (float)q.number("visibility.lod_max_relative_width_error");
    s.thinPreserveArea = q.boolean("visibility.lod_thin_preserve_area");
    s.clusterMinTriangles = (uint32_t)q.integer("visibility.cluster_min_triangles");
    s.sheetOrientationMinWidth = (float)q.number("visibility.sheet_orientation_min_width");
    if (s.clusterTriangles < 4 || s.clusterTriangles > 128) fail("visibility.cluster_triangles must be 4..128 (vis id triangle field is 7 bits)");
    if (s.clusterVertices < 3 || s.clusterVertices > 128) fail("visibility.cluster_vertices must be 3..128 (V's mesh shaders output at most 128 vertices)");
    if (s.clusterMinTriangles < 1 || s.clusterMinTriangles > s.clusterTriangles) fail("visibility.cluster_min_triangles must be 1..cluster_triangles");
    if (!(s.sheetOrientationMinWidth >= 0)) fail("visibility.sheet_orientation_min_width must be >= 0");
    s.compression = q.has("visibility.cluster_compression") && q.boolean("visibility.cluster_compression");
    s.streaming = q.has("visibility.cluster_streaming") && q.boolean("visibility.cluster_streaming");
    s.positionStep = q.has("visibility.cluster_position_step") ? (float)q.number("visibility.cluster_position_step") : 1.0f / 1024;
    s.normalBits = q.has("visibility.cluster_normal_bits") ? (uint32_t)q.integer("visibility.cluster_normal_bits") : 10u;
    s.tangentBits = q.has("visibility.cluster_tangent_bits") ? (uint32_t)q.integer("visibility.cluster_tangent_bits") : 8u;
    s.uvBits = q.has("visibility.cluster_uv_bits") ? (uint32_t)q.integer("visibility.cluster_uv_bits") : 12u;
    if (!(s.positionStep > 0)) fail("visibility.cluster_position_step must be > 0");
    if (s.normalBits < 4 || s.normalBits > 12 || s.tangentBits < 4 || s.tangentBits > 11 || s.uvBits < 4 || s.uvBits > 15)
        fail("visibility.cluster_normal_bits 4..12, cluster_tangent_bits 4..11, cluster_uv_bits 4..15 (a vertex is at most 129 bits)");
    return s;
}

namespace
{
// A11 cut faces (CutFace.hlsli, INTERFACES 8.1 Cut): bits 24..26 of a Cut-class triangle's word mark its edges on the
// cut polygon's boundary (bit i: the edge opposite corner i). At LOD 0 a boundary edge is one the submesh's own triangles
// use once (the neighbour across it has another material, or there is none), with vertices welded by exact position.
// Borders shared with another submesh are locked in simplification, but the mesh's open border is not: a simplified
// cluster may join two border vertices along it by an edge LOD 0 does not have. So an edge of a simplified cluster also
// counts when both ends are border vertices and its midpoint lies on the LOD 0 border within the cluster's own error
// (a chord along the border; a chord across a corner or the face does not).
constexpr uint32_t kCutEdgeShift = 24;

struct CutEdges
{
    std::vector<uint8_t> isCut;                 // per submesh
    std::map<std::array<uint32_t, 3>, uint32_t> weld;  // exact position bits -> welded id (Cut submeshes' vertices)
    std::unordered_set<uint64_t> border;        // welded edge keys
    std::unordered_set<uint32_t> borderVertex;  // welded ids on a border edge
    std::vector<std::array<float3, 2>> segments;  // the LOD 0 border edges
    std::vector<uint32_t> welded;               // per mesh vertex (kNone outside Cut submeshes)
    bool cutSubmesh(uint32_t s) const { return s < isCut.size() && isCut[s]; }
    static uint64_t key(uint32_t a, uint32_t b) { return a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a; }
    bool onBorder(float3 p, float tolerance) const
    {
        for (const auto& sg : segments)
        {
            const float3 d = sg[1] - sg[0];
            const float l2 = dot(d, d);
            const float t = l2 > 0 ? std::clamp(dot(p - sg[0], d) / l2, 0.0f, 1.0f) : 0.0f;
            const float3 q = sg[0] + d * t - p;
            if (dot(q, q) <= tolerance * tolerance) return true;
        }
        return false;
    }
    uint32_t flags(const unsigned int* tri, const std::vector<float3>& positions, float lodError) const
    {
        uint32_t f = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t va = tri[(i + 1) % 3], vb = tri[(i + 2) % 3];
            if (va >= welded.size() || vb >= welded.size()) continue;  // the builder's own vertex (LodVertices): on no LOD 0 border
            const uint32_t a = welded[va], b = welded[vb];
            if (a == render::gpu::kNone || b == render::gpu::kNone) continue;
            if (border.count(key(a, b))) f |= 1u << i;
            else if (borderVertex.count(a) && borderVertex.count(b))
            {
                const float3 m = (positions[va] + positions[vb]) * 0.5f;
                const float3 e = positions[vb] - positions[va];
                if (onBorder(m, std::max(lodError, 1e-6f * std::sqrt(dot(e, e))))) f |= 1u << i;
            }
        }
        return f;
    }
};

CutEdges cutFaceBorderEdges(const scene::Scene& scene, const scene::Mesh& m)
{
    CutEdges e;
    e.isCut.resize(m.submeshes.size(), 0);
    bool any = false;
    for (size_t s = 0; s < m.submeshes.size(); ++s)
    {
        const uint32_t mat = m.submeshes[s].material;
        e.isCut[s] = mat < scene.materials.size() && scene.materials[mat].cls == scene::MaterialClass::Cut;
        any = any || e.isCut[s];
    }
    if (!any) return e;
    e.welded.assign(m.positions.size(), render::gpu::kNone);
    auto weldOf = [&](uint32_t v) {
        if (e.welded[v] != render::gpu::kNone) return e.welded[v];
        const float3 p = m.positions[v];
        std::array<uint32_t, 3> bits;
        std::memcpy(bits.data(), &p, sizeof p);
        const uint32_t id = e.weld.emplace(bits, v).first->second;  // exact position identity
        return e.welded[v] = id;
    };
    std::unordered_map<uint64_t, uint32_t> uses;
    for (size_t s = 0; s < m.submeshes.size(); ++s)
    {
        if (!e.isCut[s]) continue;
        const scene::Submesh& sm = m.submeshes[s];
        for (uint32_t t = 0; t < sm.indexCount; t += 3)
            for (uint32_t i = 0; i < 3; ++i)
                ++uses[CutEdges::key(weldOf(m.indices[sm.indexOffset + t + i]), weldOf(m.indices[sm.indexOffset + t + (i + 1) % 3]))];
    }
    std::map<uint32_t, float3> at;
    for (size_t v = 0; v < m.positions.size(); ++v)
        if (e.welded[v] != render::gpu::kNone) at.emplace(e.welded[v], m.positions[v]);
    for (const auto& [k, n] : uses)
        if (n == 1)
        {
            e.border.insert(k);
            const uint32_t a = (uint32_t)(k >> 32), b = (uint32_t)k;
            e.borderVertex.insert(a);
            e.borderVertex.insert(b);
            e.segments.push_back({ at[a], at[b] });
        }
    return e;
}

// Identity of a mesh's hierarchy: everything buildMesh reads (positions, normals, uv0, indices, submesh ranges) and the
// settings. Materials are applied when the scene's buffers are assembled, so they are not part of it.
std::string meshKey(const scene::Mesh& m, const Settings& settings, const std::vector<uint32_t>& seamVertices)
{
    Sha256 h;
    auto add = [&](const auto& v) {
        const uint64_t n = v.size();
        h.update(&n, sizeof n);
        if (n) h.update(v.data(), n * sizeof(v[0]));
    };
    add(m.positions);
    add(m.normals);
    add(m.uv0);
    add(m.indices);
    const uint32_t morphed = settings.noSimplification || !m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0;
    h.update(&morphed, sizeof morphed);
    add(seamVertices);
    for (const scene::Submesh& sm : m.submeshes)
    {
        const uint32_t range[2] = { sm.indexOffset, sm.indexCount };
        h.update(range, sizeof range);
    }
    const uint32_t ints[3] = { settings.clusterTriangles, settings.clusterVertices, settings.clusterMinTriangles };
    const float floats[3] = { settings.maxRelativeWidthError, settings.widthAreaPercentile, settings.sheetOrientationMinWidth };
    const uint32_t thin = settings.thinPreserveArea;
    h.update(ints, sizeof ints);
    h.update(floats, sizeof floats);
    h.update(&thin, sizeof thin);
    // (compression moves the builder's own vertices onto the mesh's grid: a hierarchy with them is another entry. The
    // stream itself is made from the hierarchy afterwards and is in no entry)
    if (settings.compression && settings.thinPreserveArea)
    {
        const uint32_t compressed = 1;
        h.update(&compressed, sizeof compressed);
        h.update(&settings.positionStep, sizeof settings.positionStep);
    }
    const std::array<uint8_t, 32> d = h.finish();
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

// Seam vertices of every mesh (sorted mesh vertex indices): open-border vertices on the mesh's bounding box whose world
// position (any static instance) coincides with such a vertex of another instance, within 1e-5 of the coordinate
// magnitude (at least 1e-5 m). Neighbours whose cuts differ would otherwise open a crack of up to both LOD errors along
// the shared border; a mesh that meets nothing keeps its border free to simplify. Placement is part of the scene, so
// a moved static instance changes its mesh's key (the builder runs on commit; instance edits after commit do not
// rebuild hierarchies, INTERFACES 6.3).
std::vector<std::vector<uint32_t>> findSeams(const scene::Scene& scene)
{
    const uint32_t meshCount = (uint32_t)scene.meshes.size();
    std::vector<std::vector<uint32_t>> candidates(meshCount), seams(meshCount);
    std::vector<uint8_t> used(meshCount, 0);
    for (const scene::Instance& in : scene.instances)
        if (in.mesh < meshCount && !(in.flags & (scene::InstanceDynamic | scene::InstanceSkinned))) used[in.mesh] = 1;
    Jobs::instance().parallelFor(meshCount, [&](uint32_t mi) {
        const scene::Mesh& m = scene.meshes[mi];
        if (!used[mi] || m.positions.empty()) return;
        float3 lo = m.positions[0], hi = lo;
        for (const float3& p : m.positions)
        {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        // Welded by exact position; edges counted over every submesh.
        auto bitsOf = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> byPosition;
        std::vector<uint32_t> id(m.positions.size());
        for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v)
        {
            const float3 p = m.positions[v];
            id[v] = byPosition.emplace(std::make_tuple(bitsOf(p.x), bitsOf(p.y), bitsOf(p.z)), v).first->second;
        }
        std::unordered_map<uint64_t, uint32_t> edges;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        {
            const uint32_t a = id[m.indices[t]], b = id[m.indices[t + 1]], c = id[m.indices[t + 2]];
            if (a == b || b == c || a == c) continue;
            for (auto [u, w] : { std::pair{ a, b }, std::pair{ b, c }, std::pair{ c, a } }) ++edges[(uint64_t)std::min(u, w) << 32 | std::max(u, w)];
        }
        auto onBox = [&](const float3& p) { return p.x == lo.x || p.x == hi.x || p.y == lo.y || p.y == hi.y || p.z == lo.z || p.z == hi.z; };
        std::vector<uint32_t> out;
        for (const auto& [e, count] : edges)
            if (count == 1)
                for (uint32_t v : { (uint32_t)(e >> 32), (uint32_t)e })
                    if (onBox(m.positions[v])) out.push_back(v);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        candidates[mi] = std::move(out);
    });
    // World positions of every static instance's candidates on a grid of cells twice the tolerance; matches are searched
    // in the 27 neighbouring cells.
    struct Point
    {
        double x, y, z;
        uint32_t instance, mesh, vertex;
    };
    std::vector<Point> points;
    for (uint32_t ii = 0; ii < (uint32_t)scene.instances.size(); ++ii)
    {
        const scene::Instance& in = scene.instances[ii];
        if (in.mesh >= meshCount || (in.flags & (scene::InstanceDynamic | scene::InstanceSkinned))) continue;
        const float3x4& t = in.transform;
        for (uint32_t v : candidates[in.mesh])
        {
            const float3 p = scene.meshes[in.mesh].positions[v];
            double w[3];
            for (int r = 0; r < 3; ++r) w[r] = (double)t.m[r][0] * p.x + (double)t.m[r][1] * p.y + (double)t.m[r][2] * p.z + t.m[r][3];
            points.push_back({ w[0], w[1], w[2], ii, in.mesh, v });
        }
    }
    double extent = 1.0;
    for (const Point& p : points) extent = std::max({ extent, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z) });
    const double tolerance = std::max(1e-5, extent * 1e-5), cell = 2 * tolerance;
    auto cellOf = [&](double c) { return (int64_t)std::floor(c / cell); };
    auto key = [](int64_t x, int64_t y, int64_t z) { return (uint64_t)(x * 73856093) ^ (uint64_t)(y * 19349663) ^ (uint64_t)(z * 83492791); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t k = 0; k < (uint32_t)points.size(); ++k) grid[key(cellOf(points[k].x), cellOf(points[k].y), cellOf(points[k].z))].push_back(k);
    std::vector<uint8_t> seam(points.size(), 0);
    Jobs::instance().parallelFor((uint32_t)points.size(), [&](uint32_t k) {
        const Point& p = points[k];
        const int64_t cx = cellOf(p.x), cy = cellOf(p.y), cz = cellOf(p.z);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (uint32_t o : it->second)
                    {
                        const Point& q = points[o];
                        if (q.instance != p.instance && std::fabs(q.x - p.x) <= tolerance && std::fabs(q.y - p.y) <= tolerance && std::fabs(q.z - p.z) <= tolerance)
                        {
                            seam[k] = 1;
                            return;
                        }
                    }
                }
    });
    for (uint32_t k = 0; k < (uint32_t)points.size(); ++k)
        if (seam[k]) seams[points[k].mesh].push_back(points[k].vertex);
    for (std::vector<uint32_t>& list : seams)
    {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    return seams;
}

// Hierarchies of the previous build, by mesh identity (D0 editing: a re-commit of an edited level rebuilds only the meshes
// that are new or changed). The cache keeps exactly the meshes of the latest build.
std::mutex g_meshCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const MeshOut>> g_meshCache;
std::string g_diskCache;  // empty: no disk cache
bool g_diskCacheSet = false;

// ---- Disk cache (C1: a reopened project or an editor reload rebuilds nothing that was built before) ---------------------
// File <dir>/clusters/<key hex>.unxcl: magic, format version, the builder's source hash (UNX_CLUSTERBUILDER_SOURCE_HASH:
// SHA-256 of this builder's sources and meshoptimizer's, so any code change invalidates every entry), the mesh key, the
// payload, and the payload's SHA-256 (a torn or corrupted file is rebuilt, never used). Written to a temporary file and
// renamed, so readers never see a partial entry.
constexpr uint32_t kDiskMagic = 0x4C43584Eu;  // "NXCL"
constexpr uint32_t kDiskFormat = 2;  // 2: the builder's own vertices (LodVertices)

std::string hexOf(const std::string& key)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : key)
    {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string diskCacheDirectory()
{
    {
        std::lock_guard lock(g_meshCacheMutex);
        if (g_diskCacheSet) return g_diskCache;
    }
    // Default: UNX_COOK_CACHE (the Unity editor sets it to Library/UnravelNextCook before the renderer loads content).
    // GetEnvironmentVariableW sees values the process set after start-up, unlike the CRT's copy.
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(L"UNX_COOK_CACHE", buffer, 1024);
    if (n == 0 || n >= 1024) return {};
    return std::filesystem::path(std::wstring(buffer, n)).string();
}

struct Writer
{
    std::vector<uint8_t> bytes;
    void raw(const void* p, size_t n) { bytes.insert(bytes.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof v); }
    template <class T> void vec(const std::vector<T>& v)
    {
        pod((uint64_t)v.size());
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }
};

struct Reader
{
    const uint8_t* p;
    size_t left;
    bool ok = true;
    void raw(void* out, size_t n)
    {
        if (n > left)
        {
            ok = false;
            return;
        }
        std::memcpy(out, p, n);
        p += n;
        left -= n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
    template <class T> void vec(std::vector<T>& v)
    {
        uint64_t n = 0;
        pod(n);
        if (!ok || n > left / sizeof(T))
        {
            ok = false;
            return;
        }
        v.resize((size_t)n);
        if (n) raw(v.data(), (size_t)n * sizeof(T));
    }
};

std::vector<uint8_t> serialize(const MeshOut& mo)
{
    Writer w;
    w.pod((uint64_t)mo.clusters.size());
    for (const ClusterOut& c : mo.clusters)
    {
        w.vec(c.indices);
        w.pod(c.refined);
        w.pod(c.lod);
        w.pod(c.submesh);
        w.pod(c.width);
    }
    w.vec(mo.groups);
    w.vec(mo.nodes);
    w.pod(mo.levelCount);
    w.pod(mo.stats);
    w.vec(mo.lodPositions);
    w.vec(mo.lodSources);
    return std::move(w.bytes);
}

bool deserialize(const uint8_t* p, size_t n, MeshOut& mo)
{
    Reader r{ p, n };
    uint64_t clusters = 0;
    r.pod(clusters);
    if (!r.ok || clusters > n) return false;
    mo.clusters.resize((size_t)clusters);
    for (ClusterOut& c : mo.clusters)
    {
        r.vec(c.indices);
        r.pod(c.refined);
        r.pod(c.lod);
        r.pod(c.submesh);
        r.pod(c.width);
        if (!r.ok) return false;
    }
    r.vec(mo.groups);
    r.vec(mo.nodes);
    r.pod(mo.levelCount);
    r.pod(mo.stats);
    r.vec(mo.lodPositions);
    r.vec(mo.lodSources);
    return r.ok && r.left == 0;
}

std::filesystem::path diskPath(const std::string& dir, const std::string& key)
{
    return std::filesystem::path(dir) / "clusters" / (hexOf(key) + ".unxcl");
}

// Header: magic, format, source hash (32), key (32); then the payload; then the payload's SHA-256 (32).
constexpr size_t kHeaderBytes = 8 + 32 + 32;

std::string sourceHash()
{
    // UNX_CLUSTERBUILDER_SOURCE_HASH is 64 hex digits; the file stores its 32 bytes.
    const char* hex = UNX_CLUSTERBUILDER_SOURCE_HASH;
    std::string out(32, '\0');
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (int i = 0; i < 32; ++i) out[i] = (char)(v(hex[2 * i]) * 16 + v(hex[2 * i + 1]));
    return out;
}

std::shared_ptr<const MeshOut> loadFromDisk(const std::string& dir, const std::string& key)
{
    std::ifstream f(diskPath(dir, key), std::ios::binary);
    if (!f) return nullptr;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < kHeaderBytes + 32) return nullptr;
    uint32_t head[2];
    std::memcpy(head, file.data(), 8);
    const std::string source = sourceHash();
    if (head[0] != kDiskMagic || head[1] != kDiskFormat || std::memcmp(file.data() + 8, source.data(), 32) != 0 ||
        std::memcmp(file.data() + 40, key.data(), 32) != 0)
        return nullptr;
    const size_t payload = file.size() - kHeaderBytes - 32;
    Sha256 h;
    h.update(file.data() + kHeaderBytes, payload);
    const std::array<uint8_t, 32> d = h.finish();
    if (std::memcmp(d.data(), file.data() + kHeaderBytes + payload, 32) != 0)
    {
        logf("UnravelNext cluster cache: %s is corrupted (checksum); rebuilding it\n", diskPath(dir, key).string().c_str());
        return nullptr;
    }
    auto mo = std::make_shared<MeshOut>();
    if (!deserialize(file.data() + kHeaderBytes, payload, *mo)) return nullptr;
    return mo;
}

void storeToDisk(const std::string& dir, const std::string& key, const MeshOut& mo)
{
    std::error_code ec;
    const std::filesystem::path path = diskPath(dir, key);
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::vector<uint8_t> payload = serialize(mo);
    Sha256 h;
    h.update(payload.data(), payload.size());
    const std::array<uint8_t, 32> d = h.finish();
    const uint32_t head[2] = { kDiskMagic, kDiskFormat };
    const std::string source = sourceHash();
    const std::filesystem::path tmp = path.string() + ".tmp" + std::to_string(GetCurrentThreadId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            logf("UnravelNext cluster cache: cannot write %s\n", tmp.string().c_str());
            return;
        }
        f.write((const char*)head, 8);
        f.write(source.data(), 32);
        f.write(key.data(), 32);
        f.write((const char*)payload.data(), (std::streamsize)payload.size());
        f.write((const char*)d.data(), 32);
        if (!f)
        {
            logf("UnravelNext cluster cache: writing %s failed\n", tmp.string().c_str());
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}
} // namespace

void clearMeshCache()
{
    std::lock_guard lock(g_meshCacheMutex);
    g_meshCache.clear();
}

void setDiskCache(const std::string& directory)
{
    std::lock_guard lock(g_meshCacheMutex);
    g_diskCache = directory;
    g_diskCacheSet = true;
}

void LodVertices::appendTo(scene::Scene& scene) const
{
    for (size_t mi = 0; mi < meshes.size() && mi < scene.meshes.size(); ++mi)
    {
        scene::Mesh& m = scene.meshes[mi];
        const Mesh& lod = meshes[mi];
        const size_t count = m.positions.size();
        const bool normals = m.normals.size() == count, tangents = m.tangents.size() == count, uv0 = m.uv0.size() == count;
        const bool skin = m.skin.joints.size() == 4 * count && m.skin.weights.size() == 4 * count;
        for (size_t i = 0; i < lod.positions.size(); ++i)
        {
            const uint32_t s = lod.source[i];  // (its streams are copied by value below: the vectors grow)
            m.positions.push_back(lod.positions[i]);
            if (normals) m.normals.push_back(float3(m.normals[s]));
            if (tangents) m.tangents.push_back(float4(m.tangents[s]));
            if (uv0) m.uv0.push_back(float2(m.uv0[s]));
            if (skin)
                for (uint32_t k = 0; k < 4; ++k)
                {
                    m.skin.joints.push_back(uint16_t(m.skin.joints[4 * s + k]));
                    m.skin.weights.push_back(float(m.skin.weights[4 * s + k]));
                }
        }
    }
}

render::ClusterData build(const scene::Scene& scene, const Settings& requested, BuildStats* stats, LodVertices* lodVertices, StreamPages* pages)
{
    StreamGroups streamGroups;  // (the groups as the clusters are put together below: the stream's pages are whole groups)
    const auto t0 = std::chrono::steady_clock::now();
    // Without a place for the builder's own vertices, no cluster may index one: thin geometry stays as it is.
    Settings settings = requested;
    if (!lodVertices) settings.thinPreserveArea = false;
    const uint32_t meshCount = (uint32_t)scene.meshes.size();
    std::vector<std::string> keys(meshCount);
    const std::vector<std::vector<uint32_t>> seams = findSeams(scene);
    Jobs::instance().parallelFor(meshCount, [&](uint32_t i) { keys[i] = meshKey(scene.meshes[i], settings, seams[i]); });
    std::vector<std::shared_ptr<const MeshOut>> built(meshCount);
    {
        std::lock_guard lock(g_meshCacheMutex);
        for (uint32_t i = 0; i < meshCount; ++i)
            if (auto it = g_meshCache.find(keys[i]); it != g_meshCache.end()) built[i] = it->second;
    }
    std::vector<uint32_t> missing;
    for (uint32_t i = 0; i < meshCount; ++i)
        if (!built[i]) missing.push_back(i);
    // Duplicate meshes in one scene build once.
    std::unordered_map<std::string, uint32_t> firstOf;
    std::vector<uint32_t> toBuild;
    for (uint32_t i : missing)
        if (firstOf.emplace(keys[i], i).second) toBuild.push_back(i);
    const std::string disk = diskCacheDirectory();
    std::vector<uint8_t> fromDisk(toBuild.size(), 0);
    Jobs::instance().parallelFor((uint32_t)toBuild.size(), [&](uint32_t k) {
        const uint32_t i = toBuild[k];
        if (!disk.empty())
            if (auto cached = loadFromDisk(disk, keys[i]))
            {
                built[i] = std::move(cached);
                fromDisk[k] = 1;
                return;
            }
        auto mo = std::make_shared<const MeshOut>(buildMesh(scene.meshes[i], settings, seams[i]));
        if (!disk.empty()) storeToDisk(disk, keys[i], *mo);
        built[i] = std::move(mo);
    });
    uint32_t diskMeshes = 0;
    for (uint8_t f : fromDisk) diskMeshes += f;
    for (uint32_t i : missing) built[i] = built[firstOf[keys[i]]];
    {
        std::lock_guard lock(g_meshCacheMutex);
        g_meshCache.clear();
        for (uint32_t i = 0; i < meshCount; ++i) g_meshCache[keys[i]] = built[i];
    }
    const uint32_t reused = meshCount - (uint32_t)toBuild.size();
    std::vector<MeshStats> meshStats(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i) meshStats[i] = built[i]->stats;

    if (lodVertices)
    {
        lodVertices->meshes.assign(meshCount, {});
        for (uint32_t i = 0; i < meshCount; ++i) lodVertices->meshes[i] = { built[i]->lodPositions, built[i]->lodSources };
    }

    render::ClusterData data;
    data.meshes.resize(scene.meshes.size());
    // Named buffers exist even when empty so V can always bind them.
    appendNamed(data, kClusterNodes, nullptr, 0, sizeof(gpu::ClusterNode));
    appendNamed(data, kMeshClusterRoots, nullptr, 0, sizeof(gpu::MeshClusterRoots));
    appendNamed(data, kClusterLodSpheres, nullptr, 0, sizeof(float4));
    appendNamed(data, kClusterSheets, nullptr, 0, sizeof(float4));
    uint32_t nodeBase = 0;
    for (size_t mi = 0; mi < built.size(); ++mi)
    {
        const MeshOut& mo = *built[mi];
        const scene::Mesh& m = scene.meshes[mi];
        const uint32_t clusterBase = (uint32_t)data.clusters.size();
        const CutEdges cutEdges = cutFaceBorderEdges(scene, m);
        // What the clusters index: the mesh's vertices, then the builder's own (LodVertices).
        std::vector<float3> withLod;
        if (!mo.lodPositions.empty())
        {
            withLod = m.positions;
            withLod.insert(withLod.end(), mo.lodPositions.begin(), mo.lodPositions.end());
        }
        const std::vector<float3>& positions = mo.lodPositions.empty() ? m.positions : withLod;
        render::ClusterData::MeshRange& range = data.meshes[mi];
        range.clusterOffset = clusterBase;
        range.clusterCount = (uint32_t)mo.clusters.size();
        {
            // Cut bound: a cut takes, per group of the DAG, either the group's clusters or (recursively) what its
            // simplification made; when no simplification made more clusters than its group had, no cut holds more than
            // the leaves (the source clusters). Checked here per group; a mesh where it fails is bounded by all its clusters.
            std::vector<uint32_t> made(mo.groups.size(), 0);
            uint32_t leaves = 0;
            for (const ClusterOut& c : mo.clusters)
            {
                if (c.refined < 0) ++leaves;
                else if ((size_t)c.refined < made.size()) ++made[c.refined];
            }
            bool shrinks = true;
            for (size_t gi = 0; gi < mo.groups.size(); ++gi) shrinks = shrinks && made[gi] <= mo.groups[gi].clusterCount;
            range.cutBound = shrinks ? leaves : range.clusterCount;
            if (!shrinks)
                logf("cluster builder: mesh %zu - a group's simplification made more clusters than the group had: its cut bound is all %u clusters\n", mi,
                     range.clusterCount);
        }

        // Clusters (group order), their LOD spheres and index pools.
        for (size_t gi = 0; gi < mo.groups.size(); ++gi)
        {
            const GroupOut& g = mo.groups[gi];
            for (uint32_t k = 0; k < g.clusterCount; ++k)
            {
                const ClusterOut& c = mo.clusters[g.firstCluster + k];
                const size_t indexCount = c.indices.size();
                const meshopt_Bounds b = meshopt_computeClusterBounds(c.indices.data(), indexCount, &positions[0].x, positions.size(), sizeof(float3));
                unsigned int localVertices[256];
                unsigned char localTriangles[3 * 256];
                const size_t vertexCount = clodLocalIndices(localVertices, localTriangles, c.indices.data(), indexCount);
                const size_t triangleCount = indexCount / 3;
                if (vertexCount > 255 || triangleCount > 128) fail("cluster builder: cluster with %zu vertices / %zu triangles", vertexCount, triangleCount);
                render::gpu::Cluster rc{};
                rc.boundsSphere = { b.center[0], b.center[1], b.center[2], b.radius };
                rc.normalCone = { b.cone_axis[0], b.cone_axis[1], b.cone_axis[2], b.cone_cutoff };
                rc.vertexOffset = (uint32_t)data.clusterVertexIndices.size();
                rc.triangleOffset = (uint32_t)data.clusterTriangles.size();
                rc.counts = (uint32_t)vertexCount | (uint32_t)triangleCount << 8 | c.submesh << 16;
                rc.material = m.submeshes[c.submesh].material;
                rc.lodError = c.refined < 0 ? 0.0f : c.lod.error;
                rc.parentLodError = g.simplified.error;
                rc.minFeatureWidth = c.width;
                rc.brick = render::gpu::kNone;
                data.clusterVertexIndices.insert(data.clusterVertexIndices.end(), localVertices, localVertices + vertexCount);
                const bool cut = cutEdges.cutSubmesh(c.submesh);
                for (size_t t = 0; t < triangleCount; ++t)
                    data.clusterTriangles.push_back((uint32_t)localTriangles[3 * t] | (uint32_t)localTriangles[3 * t + 1] << 8 | (uint32_t)localTriangles[3 * t + 2] << 16 |
                                                    (cut ? cutEdges.flags(&c.indices[3 * t], positions, rc.lodError) << kCutEdgeShift : 0u));
                data.clusters.push_back(rc);
                streamGroups.clusterGroup.push_back((uint32_t)(streamGroups.groups.size() + gi));
                streamGroups.clusterRefined.push_back(c.refined < 0 ? render::gpu::kNone : (uint32_t)(streamGroups.groups.size() + (size_t)c.refined));
                const float4 sphere{ c.lod.center[0], c.lod.center[1], c.lod.center[2], c.lod.radius };
                appendNamed(data, kClusterLodSpheres, &sphere, sizeof sphere, sizeof(float4));
                const SheetOrientation sheet = sheetOrientation(positions, c.indices.data(), indexCount);
                const float3 centre{ b.center[0], b.center[1], b.center[2] };
                float slab = 0;
                for (size_t v = 0; v < vertexCount; ++v) slab = std::max(slab, std::fabs(dot(sheet.axis, positions[localVertices[v]] - centre)));
                const float3 scaled = sheet.cosSpread > 0 ? sheet.axis * sheet.cosSpread : float3{ 0, 0, 0 };
                const float4 sheetData{ scaled.x, scaled.y, scaled.z, slab };
                appendNamed(data, kClusterSheets, &sheetData, sizeof sheetData, sizeof(float4));
            }
        }

        for (const GroupOut& g : mo.groups) streamGroups.groups.push_back({ clusterBase + g.firstCluster, g.clusterCount, g.simplified.error == FLT_MAX });

        // Hierarchy nodes.
        std::vector<gpu::ClusterNode> nodes(mo.nodes.size());
        for (size_t n = 0; n < mo.nodes.size(); ++n)
        {
            const clodNode& cn = mo.nodes[n];
            gpu::ClusterNode& gn = nodes[n];
            gn.lodSphere = { cn.bounds.center[0], cn.bounds.center[1], cn.bounds.center[2], cn.bounds.radius };
            gn.lodError = cn.bounds.error;
            if (cn.group >= 0)
            {
                gn.first = clusterBase + mo.groups[cn.group].firstCluster;
                gn.count = mo.groups[cn.group].clusterCount;
                gn.leaf = 1;
            }
            else
            {
                gn.first = nodeBase + cn.child_offset;
                gn.count = cn.child_count;
                gn.leaf = 0;
            }
        }
        appendNamed(data, kClusterNodes, nodes.data(), nodes.size() * sizeof(gpu::ClusterNode), sizeof(gpu::ClusterNode));
        {
            // The view cut bound's tables (MeshRange::cutBoundAt): own and parent errors above log-spaced edges from the
            // smallest positive error, valid when no cluster's parent error is below its own; and the sphere holding
            // every LOD sphere (clusters' and nodes').
            float base = FLT_MAX;
            bool monotone = true;
            for (uint32_t c = 0; c < range.clusterCount; ++c)
            {
                const render::gpu::Cluster& rc = data.clusters[clusterBase + c];
                if (rc.lodError > 0) base = std::min(base, rc.lodError);
                if (rc.parentLodError > 0) base = std::min(base, rc.parentLodError);
                monotone = monotone && rc.parentLodError >= rc.lodError;
            }
            if (monotone && base < FLT_MAX)
            {
                range.cutErrorBase = base;
                for (uint32_t k = 0; k < render::ClusterData::MeshRange::kCutBins; ++k)
                {
                    const float e = base * std::exp2(0.5f * (float)k);
                    for (uint32_t c = 0; c < range.clusterCount; ++c)
                    {
                        const render::gpu::Cluster& rc = data.clusters[clusterBase + c];
                        range.parentAbove[k] += rc.parentLodError > e ? 1u : 0u;
                        range.ownAbove[k] += rc.lodError > e ? 1u : 0u;
                    }
                }
            }
            float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            auto grow = [&](const float* centre) {
                lo = { std::min(lo.x, centre[0]), std::min(lo.y, centre[1]), std::min(lo.z, centre[2]) };
                hi = { std::max(hi.x, centre[0]), std::max(hi.y, centre[1]), std::max(hi.z, centre[2]) };
            };
            for (const ClusterOut& c : mo.clusters) grow(c.lod.center);
            for (const clodNode& n : mo.nodes) grow(n.bounds.center);
            const float3 centre = (lo + hi) * 0.5f;
            float radius = 0;
            auto reach = [&](const float* c, float r) { radius = std::max(radius, length(float3{ c[0], c[1], c[2] } - centre) + r); };
            for (const ClusterOut& c : mo.clusters) reach(c.lod.center, c.lod.radius);
            for (const clodNode& n : mo.nodes) reach(n.bounds.center, n.bounds.radius);
            range.lodBounds = { centre.x, centre.y, centre.z, radius };
        }
        const gpu::MeshClusterRoots roots{ nodeBase, mo.levelCount };
        appendNamed(data, kMeshClusterRoots, &roots, sizeof roots, sizeof roots);
        nodeBase += (uint32_t)nodes.size();

        // Uniform-error cuts (ray tracing geometry, diagnostics): 0 and each depth's largest finite group error.
        std::vector<float> thresholds{ 0.0f };
        for (uint32_t d = 0; d < mo.levelCount; ++d)
        {
            float t = -1;
            for (const GroupOut& g : mo.groups)
                if (g.depth == (int)d && g.simplified.error != FLT_MAX) t = std::max(t, g.simplified.error);
            if (t >= 0) thresholds.push_back(t);
        }
        std::sort(thresholds.begin(), thresholds.end());
        thresholds.erase(std::unique(thresholds.begin(), thresholds.end()), thresholds.end());
        range.lodLevelOffset = (uint32_t)data.lodLevels.size();
        uint32_t previousTriangles = UINT32_MAX;
        for (float t : thresholds)
        {
            render::gpu::LodLevel level{};
            level.clusterOffset = (uint32_t)data.lodLevelClusters.size();
            level.error = t;
            std::vector<uint32_t> cut;
            for (uint32_t c = 0; c < range.clusterCount; ++c)
            {
                const render::gpu::Cluster& rc = data.clusters[clusterBase + c];
                if (rc.lodError <= t && t < rc.parentLodError)
                {
                    cut.push_back(clusterBase + c);
                    level.triangleCount += (rc.counts >> 8) & 0xFFu;
                }
            }
            if (level.triangleCount >= previousTriangles) continue;  // keep strictly coarser cuts only
            previousTriangles = level.triangleCount;
            level.clusterCount = (uint32_t)cut.size();
            data.lodLevelClusters.insert(data.lodLevelClusters.end(), cut.begin(), cut.end());
            data.lodLevels.push_back(level);
        }
        range.lodLevelCount = (uint32_t)data.lodLevels.size() - range.lodLevelOffset;
        meshStats[mi].lodLevels = range.lodLevelCount;
        meshStats[mi].coarsestTriangles = range.lodLevelCount ? data.lodLevels.back().triangleCount : 0;
    }
    // Compressed cluster vertices: from the hierarchy as put together (global cluster indices are the handles').
    if (settings.compression && !settings.noSimplification)  // (run-time meshes are installed uncompressed: GpuScene::addRuntimeMesh)
    {
        LodVertices own;
        if (!lodVertices)
        {
            own.meshes.assign(built.size(), {});
            for (size_t i = 0; i < built.size(); ++i) own.meshes[i] = { built[i]->lodPositions, built[i]->lodSources };
        }
        encodeStream(scene, settings, lodVertices ? lodVertices : &own, streamGroups, data, settings.streaming ? pages : nullptr);
    }
    if (stats)
    {
        stats->meshes.clear();
        stats->meshes = meshStats;
        stats->reusedMeshes = reused;
        stats->diskMeshes = diskMeshes;
        stats->wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return data;
}
} // namespace unx::clusterbuilder
