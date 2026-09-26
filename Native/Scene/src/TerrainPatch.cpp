#include "unx/scene/TerrainPatch.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::scene
{
TerrainGrid terrainGrid(const Mesh& mesh)
{
    TerrainGrid g;
    g.mesh = &mesh;
    const size_t vertices = mesh.positions.size();
    const uint32_t side = (uint32_t)std::llround(std::sqrt((double)vertices));
    if (side < 2 || (size_t)side * side != vertices) fail("terrain patch: mesh '%s' has %zu vertices, not a square grid", mesh.name.c_str(), vertices);
    g.cells = side - 1;
    const float3 p0 = mesh.positions[0], pi = mesh.positions[1], pj = mesh.positions[side];
    if (pi.z != p0.z || pj.x != p0.x || pi.x == p0.x || pj.z == p0.z) fail("terrain patch: mesh '%s' is not an axis-aligned grid", mesh.name.c_str());
    g.originX = p0.x, g.originZ = p0.z;
    g.stepX = pi.x - p0.x, g.stepZ = pj.z - p0.z;
    g.solid.assign((size_t)g.cells * g.cells, 0);
    bool facingSet = false;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
    {
        const uint32_t a = std::min({ mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2] });
        const uint32_t i = a % side, j = a / side;
        if (i >= g.cells || j >= g.cells) fail("terrain patch: mesh '%s' has a triangle outside the grid cells", mesh.name.c_str());
        g.solid[(size_t)j * g.cells + i] = 1;
        if (!facingSet)
        {
            const float3 n = cross(mesh.positions[mesh.indices[t + 1]] - mesh.positions[mesh.indices[t]], mesh.positions[mesh.indices[t + 2]] - mesh.positions[mesh.indices[t]]);
            g.facing = n.y >= 0 ? 1.0f : -1.0f;
            facingSet = true;
        }
    }
    return g;
}

uint32_t texelsPerCell(const TerrainGrid& g, const TerrainDeformation& d)
{
    if (!(d.spacing > 0) || (!d.height && d.size)) fail("terrain patch: deformation window %u^2 without heights, spacing %g", d.size, d.spacing);
    const double kx = std::fabs(g.stepX) / d.spacing, kz = std::fabs(g.stepZ) / d.spacing;
    const uint32_t k = (uint32_t)std::llround(kx);
    if (k < 1 || std::fabs(kx - k) > 1e-4 || std::fabs(kz - k) > 1e-4) fail("terrain patch: cell %g x %g m is not a whole number of %g m texels", g.stepX, g.stepZ, d.spacing);
    const double ox = (g.originX - d.originX) / d.spacing, oz = (g.originZ - d.originZ) / d.spacing;
    if (std::fabs(ox - std::llround(ox)) > 1e-3 || std::fabs(oz - std::llround(oz)) > 1e-3) fail("terrain patch: the tile's grid is not on the deformation texels");
    return k;
}

namespace
{
struct Frame  // fine grid (texel steps) of the tile: gu = i k + u', texel tx = tx0 + sx gu
{
    uint32_t k = 1;
    int64_t tx0 = 0, tz0 = 0;
    int32_t sx = 1, sz = 1;
};

Frame frameOf(const TerrainGrid& g, const TerrainDeformation& d)
{
    Frame f;
    f.k = texelsPerCell(g, d);
    f.tx0 = std::llround((g.originX - d.originX) / d.spacing);
    f.tz0 = std::llround((g.originZ - d.originZ) / d.spacing);
    f.sx = g.stepX > 0 ? 1 : -1;
    f.sz = g.stepZ > 0 ? 1 : -1;
    return f;
}

float texel(const TerrainDeformation& d, const Frame& f, int64_t gu, int64_t gv)
{
    const int64_t tx = f.tx0 + f.sx * gu, tz = f.tz0 + f.sz * gv;
    if (tx < 0 || tz < 0 || tx >= d.size || tz >= d.size) return 0.0f;
    return d.height[(size_t)tz * d.size + (size_t)tx];
}
} // namespace

bool patchBlockActive(const TerrainGrid& g, const TerrainDeformation& d, int32_t bi, int32_t bj)
{
    const Frame f = frameOf(g, d);
    const int64_t n = (int64_t)kPatchBlockCells * f.k;
    for (int64_t v = 0; v <= n; ++v)
        for (int64_t u = 0; u <= n; ++u)
            if (texel(d, f, bi * n + u, bj * n + v) != 0.0f) return true;
    return false;
}

uint32_t patchBlocksPerSide(const TerrainGrid& g)
{
    if (g.cells % kPatchBlockCells) fail("terrain patch: %u cells per tile side is not a multiple of %u", g.cells, kPatchBlockCells);
    return g.cells / kPatchBlockCells;
}

std::vector<uint32_t> activePatchBlocks(const TerrainGrid& g, const TerrainDeformation& d)
{
    const Frame f = frameOf(g, d);
    const uint32_t blocks = patchBlocksPerSide(g);
    const int64_t n = (int64_t)kPatchBlockCells * f.k, fine = (int64_t)g.cells * f.k;
    std::vector<uint8_t> active((size_t)blocks * blocks, 0);
    // Every non-zero texel inside the tile marks the blocks whose closure holds it (two or four on block lines).
    for (uint32_t tz = 0; tz < d.size; ++tz)
        for (uint32_t tx = 0; tx < d.size; ++tx)
        {
            if (d.height[(size_t)tz * d.size + tx] == 0.0f) continue;
            const int64_t gu = ((int64_t)tx - f.tx0) * f.sx, gv = ((int64_t)tz - f.tz0) * f.sz;
            if (gu < 0 || gv < 0 || gu > fine || gv > fine) continue;
            for (int64_t bj : { gv / n, (gv % n == 0) ? gv / n - 1 : gv / n })
                for (int64_t bi : { gu / n, (gu % n == 0) ? gu / n - 1 : gu / n })
                    if (bi >= 0 && bj >= 0 && bi < blocks && bj < blocks) active[(size_t)bj * blocks + bi] = 1;
        }
    std::vector<uint32_t> out;
    for (uint32_t b = 0; b < (uint32_t)active.size(); ++b)
        if (active[b]) out.push_back(b);
    return out;
}

uint64_t patchBlockHash(const TerrainGrid& g, const TerrainDeformation& d, uint32_t bi, uint32_t bj)
{
    const Frame f = frameOf(g, d);
    const int64_t n = (int64_t)kPatchBlockCells * f.k;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t v) {
        for (int b = 0; b < 4; ++b) h = (h ^ ((v >> (8 * b)) & 0xFFu)) * 1099511628211ull;
    };
    for (int64_t v = -1; v <= n + 1; ++v)
        for (int64_t u = -1; u <= n + 1; ++u)
        {
            const float t = texel(d, f, (int64_t)bi * n + u, (int64_t)bj * n + v);
            uint32_t bits;
            std::memcpy(&bits, &t, 4);
            mix(bits);
        }
    mix((patchBlockActive(g, d, (int32_t)bi - 1, (int32_t)bj) ? 1u : 0u) | (patchBlockActive(g, d, (int32_t)bi + 1, (int32_t)bj) ? 2u : 0u) |
        (patchBlockActive(g, d, (int32_t)bi, (int32_t)bj - 1) ? 4u : 0u) | (patchBlockActive(g, d, (int32_t)bi, (int32_t)bj + 1) ? 8u : 0u));
    return h;
}

Mesh buildTerrainPatch(const TerrainGrid& g, const TerrainDeformation& d, uint32_t bi, uint32_t bj)
{
    const Mesh& src = *g.mesh;
    const Frame f = frameOf(g, d);
    const uint32_t blocks = patchBlocksPerSide(g);
    if (bi >= blocks || bj >= blocks) fail("terrain patch: block (%u, %u) outside the tile's %u^2 blocks", bi, bj, blocks);
    const uint32_t k = f.k, n = kPatchBlockCells * k, side = g.cells + 1;
    const int64_t u0 = (int64_t)bi * n, v0 = (int64_t)bj * n;
    const bool hasUv = !src.uv0.empty(), hasTangents = !src.tangents.empty();
    // Stitched sides (neighbour block not replaced): left, right, bottom (v = 0), top (v = n).
    const bool stitched[4] = { !patchBlockActive(g, d, (int32_t)bi - 1, (int32_t)bj), !patchBlockActive(g, d, (int32_t)bi + 1, (int32_t)bj),
                               !patchBlockActive(g, d, (int32_t)bi, (int32_t)bj - 1), !patchBlockActive(g, d, (int32_t)bi, (int32_t)bj + 1) };
    Mesh out;
    out.name = src.name + " patch " + std::to_string(bi) + "," + std::to_string(bj);
    std::vector<int32_t> index((size_t)(n + 1) * (n + 1), -1);
    // Vertex at fine coordinates (u, v) of the block: source surface (barycentric in the source triangle) plus D.
    auto vertex = [&](uint32_t u, uint32_t v) -> uint32_t {
        int32_t& slot = index[(size_t)v * (n + 1) + u];
        if (slot >= 0) return (uint32_t)slot;
        const int64_t gu = u0 + u, gv = v0 + v;
        const uint32_t i = (uint32_t)std::min<int64_t>(gu / k, g.cells - 1), j = (uint32_t)std::min<int64_t>(gv / k, g.cells - 1);
        const uint32_t fuN = (uint32_t)(gu - (int64_t)i * k), fvN = (uint32_t)(gv - (int64_t)j * k);  // 0..k
        const uint32_t a = j * side + i, b = a + 1, c = a + side, dd = c + 1;
        uint32_t corner[3];
        float w[3];
        const float fu = (float)fuN / k, fv = (float)fvN / k;
        if (fvN >= fuN)  // triangle (a c d): a (0,0), c (0,1), d (1,1)
        {
            corner[0] = a, corner[1] = c, corner[2] = dd;
            w[0] = 1 - fv, w[1] = fv - fu, w[2] = fu;
        }
        else  // triangle (a d b): a (0,0), d (1,1), b (1,0)
        {
            corner[0] = a, corner[1] = dd, corner[2] = b;
            w[0] = 1 - fu, w[1] = fv, w[2] = fu - fv;
        }
        // Exactly on a source vertex: take it bit for bit.
        int32_t exact = -1;
        if ((fuN == 0 || fuN == k) && (fvN == 0 || fvN == k)) exact = (int32_t)((j + (fvN == k)) * side + i + (fuN == k));
        float3 p{}, nrm{};
        float2 uv{};
        float4 tangent{};
        if (exact >= 0)
        {
            p = src.positions[exact];
            nrm = src.normals[exact];
            if (hasUv) uv = src.uv0[exact];
            if (hasTangents) tangent = src.tangents[exact];
        }
        else
        {
            for (int q = 0; q < 3; ++q)
            {
                p = p + src.positions[corner[q]] * w[q];
                nrm = nrm + src.normals[corner[q]] * w[q];
                if (hasUv) uv.x += src.uv0[corner[q]].x * w[q], uv.y += src.uv0[corner[q]].y * w[q];
                if (hasTangents)
                    tangent.x += src.tangents[corner[q]].x * w[q], tangent.y += src.tangents[corner[q]].y * w[q], tangent.z += src.tangents[corner[q]].z * w[q];
            }
            nrm = normalize(nrm);
            if (hasTangents) tangent.w = src.tangents[corner[0]].w;
        }
        const float h = texel(d, f, gu, gv);
        p.y += h;
        // Normal of y = source + D: n' ~ n + n.y (-dD/dx, 0, -dD/dz) (object-space gradient by central differences).
        const float dx = (texel(d, f, gu + f.sx, gv) - texel(d, f, gu - f.sx, gv)) / (2 * d.spacing);
        const float dz = (texel(d, f, gu, gv + f.sz) - texel(d, f, gu, gv - f.sz)) / (2 * d.spacing);
        if (dx != 0 || dz != 0) nrm = normalize(nrm + float3{ -dx, 0, -dz } * nrm.y);
        if (hasTangents && (exact < 0 || dx != 0 || dz != 0))
        {
            float3 t{ tangent.x, tangent.y, tangent.z };
            t = normalize(t - nrm * dot(t, nrm));
            tangent.x = t.x, tangent.y = t.y, tangent.z = t.z;
        }
        slot = (int32_t)out.positions.size();
        out.positions.push_back(p);
        out.normals.push_back(nrm);
        if (hasUv) out.uv0.push_back(uv);
        if (hasTangents) out.tangents.push_back(tangent);
        return (uint32_t)slot;
    };
    // Cell of a triangle (by centroid) for holes, and the facing of the source.
    auto emit = [&](uint32_t a, uint32_t b, uint32_t c, float cu, float cv) {
        const uint32_t i = (uint32_t)std::min<int64_t>((int64_t)std::floor(u0 + cu) / k, g.cells - 1);
        const uint32_t j = (uint32_t)std::min<int64_t>((int64_t)std::floor(v0 + cv) / k, g.cells - 1);
        if (!g.solid[(size_t)j * g.cells + i]) return;
        const float3 nrm = cross(out.positions[b] - out.positions[a], out.positions[c] - out.positions[a]);
        if ((nrm.y >= 0) == (g.facing > 0)) out.indices.insert(out.indices.end(), { a, b, c });
        else out.indices.insert(out.indices.end(), { a, c, b });
    };
    // Inner grid [1, n - 1]^2: quads split along the source diagonal direction (u, v) -> (u + 1, v + 1).
    for (uint32_t v = 1; v + 1 < n; ++v)
        for (uint32_t u = 1; u + 1 < n; ++u)
        {
            const uint32_t a = vertex(u, v), b = vertex(u + 1, v), c = vertex(u, v + 1), dd = vertex(u + 1, v + 1);
            emit(a, c, dd, u + 0.34f, v + 0.66f);
            emit(a, dd, b, u + 0.66f, v + 0.34f);
        }
    // Ring between the outer boundary (all texel vertices on replaced-neighbour sides, only source vertices on stitched
    // sides) and the inner loop at distance 1, zipped by perimeter position (both loops start at their (0,0) corner and run
    // counter-clockwise in (u, v): bottom, right, top, left).
    struct LoopVertex
    {
        uint32_t u, v;
        double t;  // perimeter fraction
    };
    auto loop = [&](uint32_t lo, uint32_t hi, bool outer) {
        std::vector<LoopVertex> l;
        const uint32_t len = hi - lo;
        const double per = 4.0 * len;
        auto keep = [&](int sideIndex, uint32_t coord) { return !outer || !stitched[sideIndex] || ((coord - lo) % k == 0); };
        for (uint32_t s = 0; s < len; ++s)  // bottom: (lo + s, lo), side 2
            if (keep(2, lo + s)) l.push_back({ lo + s, lo, s / per });
        for (uint32_t s = 0; s < len; ++s)  // right: (hi, lo + s), side 1
            if (keep(1, lo + s)) l.push_back({ hi, lo + s, (len + s) / per });
        for (uint32_t s = 0; s < len; ++s)  // top: (hi - s, hi), side 3
            if (keep(3, hi - s)) l.push_back({ hi - s, hi, (2.0 * len + s) / per });
        for (uint32_t s = 0; s < len; ++s)  // left: (lo, hi - s), side 0
            if (keep(0, hi - s)) l.push_back({ lo, hi - s, (3.0 * len + s) / per });
        return l;
    };
    const std::vector<LoopVertex> outerLoop = loop(0, n, true), innerLoop = loop(1, n - 1, false);
    size_t oi = 0, ii = 0;
    const size_t on = outerLoop.size(), in = innerLoop.size();
    while (oi < on || ii < in)
    {
        const LoopVertex& o0 = outerLoop[oi % on];
        const LoopVertex& i0 = innerLoop[ii % in];
        const double to = oi + 1 <= on ? (oi + 1 < on ? outerLoop[oi + 1].t : 1.0) : 2.0;
        const double ti = ii + 1 <= in ? (ii + 1 < in ? innerLoop[ii + 1].t : 1.0) : 2.0;
        if (to <= ti && oi < on)
        {
            const LoopVertex& o1 = outerLoop[(oi + 1) % on];
            emit(vertex(o0.u, o0.v), vertex(o1.u, o1.v), vertex(i0.u, i0.v), (o0.u + o1.u + i0.u) / 3.0f, (o0.v + o1.v + i0.v) / 3.0f);
            ++oi;
        }
        else
        {
            const LoopVertex& i1 = innerLoop[(ii + 1) % in];
            emit(vertex(o0.u, o0.v), vertex(i1.u, i1.v), vertex(i0.u, i0.v), (o0.u + i1.u + i0.u) / 3.0f, (o0.v + i1.v + i0.v) / 3.0f);
            ++ii;
        }
    }
    out.submeshes.push_back({ 0, (uint32_t)out.indices.size(), src.submeshes.empty() ? 0u : src.submeshes[0].material });
    return out;
}
} // namespace unx::scene
