#include "unx/clusterbuilder/Bricks.h"

#include "FeatureWidth.h"
#include "unx/core/File.h"
#include "unx/core/Jobs.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace unx::clusterbuilder
{
namespace
{
constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kDirections = 13;
constexpr uint32_t kCacheVersion = 1;  // bump when the bake or its encodings change

// The 26 face, edge and corner directions up to sign, normalised.
const std::array<float3, kDirections>& directions()
{
    static const std::array<float3, kDirections> d = [] {
        std::array<float3, kDirections> out{};
        uint32_t n = 0;
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                {
                    // One of each +-pair: the first nonzero component positive.
                    const int first = x != 0 ? x : y != 0 ? y : z;
                    if (first <= 0) continue;
                    out[n++] = normalize(float3{ (float)x, (float)y, (float)z });
                }
        return out;
    }();
    return d;
}

// IEEE half from float (round to nearest even; no denormal flush issues for the shape range [-1, 1]).
uint16_t toHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0)
    {
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exponent);
        uint32_t h = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (h & 1u))) ++h;
        return (uint16_t)(sign | h);
    }
    if (exponent >= 31) return (uint16_t)(sign | 0x7C00u);
    uint32_t h = ((uint32_t)exponent << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return (uint16_t)(sign | h);
}

struct Sym3  // symmetric 3 x 3: xx, yy, zz, xy, xz, yz
{
    double m[6] = {};
    double quad(float3 w) const
    {
        return m[0] * w.x * w.x + m[1] * w.y * w.y + m[2] * w.z * w.z + 2 * (m[3] * w.x * w.y + m[4] * w.x * w.z + m[5] * w.y * w.z);
    }
};

// Eigenvalues and eigenvectors of a symmetric 3 x 3 (cyclic Jacobi).
void eigen(const Sym3& s, double values[3], double vectors[3][3])
{
    double a[3][3] = { { s.m[0], s.m[3], s.m[4] }, { s.m[3], s.m[1], s.m[5] }, { s.m[4], s.m[5], s.m[2] } };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vectors[i][j] = i == j ? 1 : 0;
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::fabs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), sn = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq;
                    a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk;
                    a[q][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double vkp = vectors[k][p], vkq = vectors[k][q];
                    vectors[k][p] = c * vkp - sn * vkq;
                    vectors[k][q] = sn * vkp + c * vkq;
                }
            }
    }
    for (int i = 0; i < 3; ++i) values[i] = a[i][i];
}

// Nearest positive semi-definite matrix (negative eigenvalues set to zero); returns its largest eigenvalue.
double makePsd(Sym3& s)
{
    double values[3], v[3][3];
    eigen(s, values, v);
    double top = 0;
    Sym3 out;
    for (int e = 0; e < 3; ++e)
    {
        const double l = std::max(values[e], 0.0);
        top = std::max(top, l);
        out.m[0] += l * v[0][e] * v[0][e];
        out.m[1] += l * v[1][e] * v[1][e];
        out.m[2] += l * v[2][e] * v[2][e];
        out.m[3] += l * v[0][e] * v[1][e];
        out.m[4] += l * v[0][e] * v[2][e];
        out.m[5] += l * v[1][e] * v[2][e];
    }
    s = out;
    return top;
}

// Heitz 2015 encoding of a normalised S (largest eigenvalue 1): sigma = sqrt(diagonal), r = correlation.
void encodeShape(const Sym3& s, uint8_t out[6])
{
    auto unorm = [](double v) { return (uint8_t)std::clamp((int)std::lround(std::clamp(v, 0.0, 1.0) * 255), 0, 255); };
    const double sx = std::sqrt(std::max(s.m[0], 0.0)), sy = std::sqrt(std::max(s.m[1], 0.0)), sz = std::sqrt(std::max(s.m[2], 0.0));
    auto corr = [](double c, double a, double b) { return a * b > 1e-12 ? std::clamp(c / (a * b), -1.0, 1.0) : 0.0; };
    out[0] = unorm(sx);
    out[1] = unorm(sy);
    out[2] = unorm(sz);
    out[3] = unorm((corr(s.m[3], sx, sy) + 1) / 2);
    out[4] = unorm((corr(s.m[4], sx, sz) + 1) / 2);
    out[5] = unorm((corr(s.m[5], sy, sz) + 1) / 2);
}

Sym3 decodeShape(const uint8_t in[6])
{
    const double sx = in[0] / 255.0, sy = in[1] / 255.0, sz = in[2] / 255.0;
    const double rxy = in[3] / 255.0 * 2 - 1, rxz = in[4] / 255.0 * 2 - 1, ryz = in[5] / 255.0 * 2 - 1;
    Sym3 s;
    s.m[0] = sx * sx;
    s.m[1] = sy * sy;
    s.m[2] = sz * sz;
    s.m[3] = rxy * sx * sy;
    s.m[4] = rxz * sx * sz;
    s.m[5] = ryz * sy * sz;
    return s;
}

// Least squares for the six entries of S from rows a (13 x 6) and targets b.
bool solve6(const double rows[kDirections][6], const double b[kDirections], double x[6])
{
    double n[6][7] = {};
    for (uint32_t r = 0; r < kDirections; ++r)
        for (int i = 0; i < 6; ++i)
        {
            for (int j = 0; j < 6; ++j) n[i][j] += rows[r][i] * rows[r][j];
            n[i][6] += rows[r][i] * b[r];
        }
    for (int c = 0; c < 6; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 6; ++r)
            if (std::fabs(n[r][c]) > std::fabs(n[pivot][c])) pivot = r;
        if (std::fabs(n[pivot][c]) < 1e-18) return false;
        for (int k = 0; k < 7; ++k) std::swap(n[c][k], n[pivot][k]);
        for (int r = 0; r < 6; ++r)
        {
            if (r == c) continue;
            const double f = n[r][c] / n[c][c];
            for (int k = c; k < 7; ++k) n[r][k] -= f * n[c][k];
        }
    }
    for (int i = 0; i < 6; ++i) x[i] = n[i][6] / n[i][i];
    return true;
}

// Triangle / axis-aligned box overlap (Akenine-Moller separating axes).
bool triangleBox(float3 centre, float3 half, float3 a, float3 b, float3 c)
{
    const float3 v0 = a - centre, v1 = b - centre, v2 = c - centre;
    const float3 e[3] = { v1 - v0, v2 - v1, v0 - v2 };
    const float h[3] = { half.x, half.y, half.z };
    auto comp = [](float3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
    for (int i = 0; i < 3; ++i)  // box axes
    {
        const float lo = std::min({ comp(v0, i), comp(v1, i), comp(v2, i) }), hi = std::max({ comp(v0, i), comp(v1, i), comp(v2, i) });
        if (lo > h[i] || hi < -h[i]) return false;
    }
    const float3 n = cross(e[0], e[1]);  // triangle plane
    const float r = half.x * std::fabs(n.x) + half.y * std::fabs(n.y) + half.z * std::fabs(n.z);
    if (std::fabs(dot(n, v0)) > r) return false;
    const float3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const float3 axis = cross(axes[i], e[j]);
            const float p0 = dot(axis, v0), p1 = dot(axis, v1), p2 = dot(axis, v2);
            const float rad = half.x * std::fabs(axis.x) + half.y * std::fabs(axis.y) + half.z * std::fabs(axis.z);
            if (std::min({ p0, p1, p2 }) > rad || std::max({ p0, p1, p2 }) < -rad) return false;
        }
    return true;
}

struct Triangle
{
    float3 a, b, c;
    float2 ta, tb, tc;
    uint32_t material;
};

struct Surface  // alpha test and colour of a material
{
    const scene::Texture* texture = nullptr;  // base colour (alpha = coverage)
    float cutoff = 0;                         // 0 = opaque
    float3 baseColor{ 1, 1, 1 };
};

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Nearest texel of mip 0 (the bake measures the source coverage exactly at texel resolution).
const uint8_t* texel(const scene::Texture& t, float2 uv)
{
    auto address = [&](float c, uint32_t n) {
        float f = c * (float)n - 0.5f;
        int i = (int)std::floor(f + 0.5f);
        if (t.wrap) i = ((i % (int)n) + (int)n) % (int)n;
        else i = std::clamp(i, 0, (int)n - 1);
        return (uint32_t)i;
    };
    const uint32_t x = address(uv.x, t.width), y = address(uv.y, t.height);
    return &t.texels[((size_t)y * t.width + x) * 4];
}

// Ray-triangle intersection (Moller-Trumbore): t and barycentrics of the hit, false when none in (tMin, tMax).
bool intersect(const Triangle& tri, float3 o, float3 d, float tMin, float tMax, float& t, float& u, float& v)
{
    const float3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
    const float3 p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-20f) return false;
    const float inv = 1 / det;
    const float3 s = o - tri.a;
    u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    const float3 q = cross(s, e1);
    v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t > tMin && t < tMax;
}

struct MeshBake
{
    gpu::BrickMesh mesh{};
    std::vector<gpu::BrickLevel> levels;
    std::vector<uint32_t> grid;  // mesh-local brick indices
    std::vector<gpu::BrickHeader> headers;
    std::vector<uint8_t> density, shape;
    std::vector<uint64_t> occupancy;
    BrickMeshStats stats;
};

float percentile(std::vector<float>& v, double q)
{
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

// Area-weighted 10th percentile of the component widths of a mesh's triangles (its thin width).
float thinWidth(const scene::Mesh& m, const detail::MeshWidthContext& widths)
{
    std::vector<std::pair<float, double>> w;  // (width, area)
    double total = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        const float3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]], c = m.positions[m.indices[i + 2]];
        const double area = 0.5 * length(cross(b - a, c - a));
        if (!(area > 0)) continue;
        w.push_back({ widths.componentWidth(m.indices[i]), area });
        total += area;
    }
    if (w.empty()) return FLT_MAX;
    std::sort(w.begin(), w.end());
    double acc = 0;
    for (const auto& [width, area] : w)
        if ((acc += area) >= 0.1 * total) return width;
    return w.back().first;
}

MeshBake bakeMesh(const scene::Scene& scene, uint32_t meshIndex, const BrickSettings& settings, float width)
{
    const auto t0 = std::chrono::steady_clock::now();
    const scene::Mesh& m = scene.meshes[meshIndex];
    MeshBake out;
    out.stats.mesh = meshIndex;
    out.stats.featureWidth = width;

    // Triangles with their materials' alpha sources.
    std::vector<Surface> surfaces(scene.materials.size());
    for (size_t i = 0; i < scene.materials.size(); ++i)
    {
        const scene::Material& mat = scene.materials[i];
        surfaces[i].baseColor = mat.baseColor;
        if (mat.baseColorTexture != scene::kNone && mat.baseColorTexture < scene.textures.size())
        {
            const scene::Texture& t = scene.textures[mat.baseColorTexture];
            if ((t.format == scene::TextureFormat::Rgba8Srgb || t.format == scene::TextureFormat::Rgba8Linear) && !t.texels.empty()) surfaces[i].texture = &t;
        }
        surfaces[i].cutoff = mat.alphaCutoff;
    }
    std::vector<Triangle> tris;
    float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const scene::Submesh& sm : m.submeshes)
        for (uint32_t i = sm.indexOffset; i + 2 < sm.indexOffset + sm.indexCount; i += 3)
        {
            Triangle t;
            const uint32_t ia = m.indices[i], ib = m.indices[i + 1], ic = m.indices[i + 2];
            t.a = m.positions[ia];
            t.b = m.positions[ib];
            t.c = m.positions[ic];
            if (!(length(cross(t.b - t.a, t.c - t.a)) > 0)) continue;
            const bool uv = ia < m.uv0.size() && ib < m.uv0.size() && ic < m.uv0.size();
            t.ta = uv ? m.uv0[ia] : float2{ 0, 0 };
            t.tb = uv ? m.uv0[ib] : float2{ 0, 0 };
            t.tc = uv ? m.uv0[ic] : float2{ 0, 0 };
            t.material = sm.material;
            tris.push_back(t);
            for (float3 p : { t.a, t.b, t.c })
            {
                lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
            }
        }
    if (tris.empty()) return out;
    std::vector<float3> boxLo(tris.size()), boxHi(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
    {
        const Triangle& t = tris[i];
        boxLo[i] = { std::min({ t.a.x, t.b.x, t.c.x }), std::min({ t.a.y, t.b.y, t.c.y }), std::min({ t.a.z, t.b.z, t.c.z }) };
        boxHi[i] = { std::max({ t.a.x, t.b.x, t.c.x }), std::max({ t.a.y, t.b.y, t.c.y }), std::max({ t.a.z, t.b.z, t.c.z }) };
    }
    detail::Bvh bvh;
    bvh.build(boxLo, boxHi);
    auto query = [&](float3 qlo, float3 qhi, std::vector<uint32_t>& found) {
        found.clear();
        if (bvh.nodes.empty()) return;
        uint32_t stack[64];
        uint32_t top = 0;
        stack[top++] = 0;
        while (top)
        {
            const detail::Bvh::Node& n = bvh.nodes[stack[--top]];
            if (n.lo.x > qhi.x || n.hi.x < qlo.x || n.lo.y > qhi.y || n.hi.y < qlo.y || n.lo.z > qhi.z || n.hi.z < qlo.z) continue;
            if (n.count > 0)
            {
                for (uint32_t k = n.first; k < n.first + n.count; ++k)
                {
                    const uint32_t i = bvh.order[k];
                    if (boxLo[i].x <= qhi.x && boxHi[i].x >= qlo.x && boxLo[i].y <= qhi.y && boxHi[i].y >= qlo.y && boxLo[i].z <= qhi.z && boxHi[i].z >= qlo.z) found.push_back(i);
                }
            }
            else if (top + 2 <= 64)
            {
                stack[top++] = n.first;
                stack[top++] = n.first + 1;
            }
        }
    };

    const uint32_t edge = settings.brickEdge, voxelsPerBrick = edge * edge * edge;
    const float v0 = 4 * width;
    const float extent = std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, v0 });
    uint32_t levelCount = 1;
    while ((float)edge * v0 * (float)(1u << (levelCount - 1)) < extent && levelCount < 24) ++levelCount;
    out.mesh.boundsMin = lo;
    out.mesh.voxel0 = v0;
    out.mesh.levelCount = levelCount;
    out.stats.voxel0 = v0;
    out.stats.levels = levelCount;

    const uint32_t raysPerAxis = std::max(1u, (uint32_t)std::lround(std::sqrt((double)settings.bakeRays)));
    const auto& dirs = directions();
    std::vector<float> allR, allHalf, allBrick;

    for (uint32_t k = 0; k < levelCount; ++k)
    {
        const float voxel = v0 * (float)(1u << k), brickSize = voxel * (float)edge;
        gpu::BrickLevel level{};
        level.voxel = voxel;
        for (int a = 0; a < 3; ++a)
        {
            const float span = a == 0 ? hi.x - lo.x : a == 1 ? hi.y - lo.y : hi.z - lo.z;
            level.dims[a] = std::max(1u, (uint32_t)std::ceil(span / brickSize - 1e-6f));
        }
        const uint64_t slots = (uint64_t)level.dims[0] * level.dims[1] * level.dims[2];
        if (slots > 65536)
            fail("bricks: mesh '%s' level %u needs %llu brick slots (limit 65536): split the asset into instances", m.name.c_str(), k, (unsigned long long)slots);
        level.gridOffset = (uint32_t)out.grid.size();
        out.levels.push_back(level);
        out.grid.resize(out.grid.size() + slots, kNone);

        struct BrickOut
        {
            uint32_t slot;
            gpu::BrickHeader header{};
            std::vector<uint8_t> density, shape;
            uint64_t occupancy = 0;
            std::vector<float> r, rHalf, rBrick;
        };
        std::vector<BrickOut> bricks((size_t)slots);
        std::vector<uint8_t> used((size_t)slots, 0);
        Jobs::instance().parallelFor((uint32_t)slots, [&](uint32_t slot) {
            const uint32_t sx = slot % level.dims[0], sy = (slot / level.dims[0]) % level.dims[1], sz = slot / (level.dims[0] * level.dims[1]);
            const float3 bLo = lo + float3{ sx * brickSize, sy * brickSize, sz * brickSize }, bHi = bLo + float3{ brickSize, brickSize, brickSize };
            std::vector<uint32_t> brickTris, voxelTris;
            query(bLo, bHi, brickTris);
            if (brickTris.empty()) return;
            BrickOut& b = bricks[slot];
            b.slot = slot;
            b.density.assign(voxelsPerBrick, 0);
            b.shape.assign(voxelsPerBrick * 6, 0);
            struct VoxelFit
            {
                uint32_t index;
                float tau;             // decoded optical depth (quantised)
                float t[kDirections];  // exact transmittance, full chord
                Sym3 shape;            // decoded normalised shape (quantised)
            };
            std::vector<VoxelFit> fits;
            double albedo[3] = {}, albedoWeight = 0;
            std::vector<uint32_t> materialHits(scene.materials.size(), 0);
            const float3 half{ voxel * 0.5f, voxel * 0.5f, voxel * 0.5f };
            for (uint32_t vz = 0; vz < edge; ++vz)
                for (uint32_t vy = 0; vy < edge; ++vy)
                    for (uint32_t vx = 0; vx < edge; ++vx)
                    {
                        const float3 centre = bLo + float3{ (vx + 0.5f) * voxel, (vy + 0.5f) * voxel, (vz + 0.5f) * voxel };
                        voxelTris.clear();
                        for (uint32_t i : brickTris)
                            if (boxLo[i].x <= centre.x + half.x && boxHi[i].x >= centre.x - half.x && boxLo[i].y <= centre.y + half.y && boxHi[i].y >= centre.y - half.y &&
                                boxLo[i].z <= centre.z + half.z && boxHi[i].z >= centre.z - half.z && triangleBox(centre, half, tris[i].a, tris[i].b, tris[i].c))
                                voxelTris.push_back(i);
                        if (voxelTris.empty()) continue;
                        const uint32_t vIndex = vx + edge * (vy + edge * vz);
                        float tFull[kDirections], tHalf[kDirections];
                        double rows[kDirections][6], targets[kDirections];
                        bool opaque = false;
                        for (uint32_t d = 0; d < kDirections; ++d)
                        {
                            const float3 w = dirs[d];
                            // Orthonormal basis of the plane across w and the voxel's projection onto it.
                            const float3 helper = std::fabs(w.x) < 0.9f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
                            const float3 u = normalize(cross(helper, w)), s = cross(w, u);
                            float uLo = FLT_MAX, uHi = -FLT_MAX, sLo = FLT_MAX, sHi = -FLT_MAX;
                            for (int c = 0; c < 8; ++c)
                            {
                                const float3 corner{ (c & 1) ? half.x : -half.x, (c & 2) ? half.y : -half.y, (c & 4) ? half.z : -half.z };
                                uLo = std::min(uLo, dot(corner, u));
                                uHi = std::max(uHi, dot(corner, u));
                                sLo = std::min(sLo, dot(corner, s));
                                sHi = std::max(sHi, dot(corner, s));
                            }
                            uint32_t rays = 0, clear = 0, clearHalf = 0;
                            for (uint32_t ru = 0; ru < raysPerAxis; ++ru)
                                for (uint32_t rs = 0; rs < raysPerAxis; ++rs)
                                {
                                    // Stratified, jittered deterministically per voxel, direction and cell.
                                    uint32_t h = (vIndex * 2654435761u) ^ (slot * 40503u) ^ (d * 97u + ru * 7919u + rs * 104729u + k * 1299709u);
                                    h ^= h >> 15;
                                    h *= 0x2C1B3C6Du;
                                    h ^= h >> 12;
                                    const float ju = ((h & 0xFFFFu) + 0.5f) / 65536.0f, js = ((h >> 16) + 0.5f) / 65536.0f;
                                    const float pu = uLo + (uHi - uLo) * (ru + ju) / raysPerAxis, ps = sLo + (sHi - sLo) * (rs + js) / raysPerAxis;
                                    const float3 origin = centre + u * pu + s * ps - w * (voxel * 2);
                                    // Chord of the voxel box.
                                    float tIn = -FLT_MAX, tOut = FLT_MAX;
                                    const float oc[3] = { origin.x - centre.x, origin.y - centre.y, origin.z - centre.z }, dc[3] = { w.x, w.y, w.z };
                                    const float hc[3] = { half.x, half.y, half.z };
                                    bool miss = false;
                                    for (int a = 0; a < 3 && !miss; ++a)
                                    {
                                        if (std::fabs(dc[a]) < 1e-12f)
                                        {
                                            if (oc[a] < -hc[a] || oc[a] > hc[a]) miss = true;
                                            continue;
                                        }
                                        float ta = (-hc[a] - oc[a]) / dc[a], tb = (hc[a] - oc[a]) / dc[a];
                                        if (ta > tb) std::swap(ta, tb);
                                        tIn = std::max(tIn, ta);
                                        tOut = std::min(tOut, tb);
                                    }
                                    if (miss || !(tOut > tIn)) continue;
                                    ++rays;
                                    const float tMid = 0.5f * (tIn + tOut);
                                    float nearest = FLT_MAX;
                                    bool hitHalf = false;
                                    uint32_t nearestTri = kNone;
                                    float nearestU = 0, nearestV = 0;
                                    for (uint32_t i : voxelTris)
                                    {
                                        float t, bu, bv;
                                        if (!intersect(tris[i], origin, w, tIn, tOut, t, bu, bv)) continue;
                                        const Surface& sf = surfaces[tris[i].material];
                                        if (sf.cutoff > 0 && sf.texture)
                                        {
                                            const float2 uv{ tris[i].ta.x * (1 - bu - bv) + tris[i].tb.x * bu + tris[i].tc.x * bv,
                                                             tris[i].ta.y * (1 - bu - bv) + tris[i].tb.y * bu + tris[i].tc.y * bv };
                                            if (texel(*sf.texture, uv)[3] / 255.0f < sf.cutoff) continue;  // cut out: the ray passes
                                        }
                                        if (t >= tMid) hitHalf = true;
                                        if (t < nearest)
                                        {
                                            nearest = t;
                                            nearestTri = i;
                                            nearestU = bu;
                                            nearestV = bv;
                                        }
                                    }
                                    if (nearestTri == kNone) ++clear;
                                    else
                                    {
                                        const Triangle& tri = tris[nearestTri];
                                        const Surface& sf = surfaces[tri.material];
                                        float3 colour = sf.baseColor;
                                        if (sf.texture)
                                        {
                                            const float2 uv{ tri.ta.x * (1 - nearestU - nearestV) + tri.tb.x * nearestU + tri.tc.x * nearestV,
                                                             tri.ta.y * (1 - nearestU - nearestV) + tri.tb.y * nearestU + tri.tc.y * nearestV };
                                            const uint8_t* px = texel(*sf.texture, uv);
                                            const bool srgb = sf.texture->format == scene::TextureFormat::Rgba8Srgb;
                                            auto channel = [&](int c) { return srgb ? srgbToLinear(px[c] / 255.0f) : px[c] / 255.0f; };
                                            colour = colour * float3{ channel(0), channel(1), channel(2) };
                                        }
                                        albedo[0] += colour.x;
                                        albedo[1] += colour.y;
                                        albedo[2] += colour.z;
                                        albedoWeight += 1;
                                        ++materialHits[tri.material];
                                    }
                                    if (!hitHalf) ++clearHalf;
                                }
                            if (rays == 0)  // cannot happen for a voxel cross-section; keep the fit well posed
                            {
                                tFull[d] = tHalf[d] = 1;
                            }
                            else
                            {
                                tFull[d] = (float)clear / rays;
                                tHalf[d] = (float)clearHalf / rays;
                            }
                            const double tClamped = std::max((double)tFull[d], 0.5 / std::max(rays, 1u));
                            const double tau = -std::log(tClamped);
                            const double chord = voxel / (std::fabs(w.x) + std::fabs(w.y) + std::fabs(w.z));
                            rows[d][0] = w.x * w.x;
                            rows[d][1] = w.y * w.y;
                            rows[d][2] = w.z * w.z;
                            rows[d][3] = 2.0 * w.x * w.y;
                            rows[d][4] = 2.0 * w.x * w.z;
                            rows[d][5] = 2.0 * w.y * w.z;
                            targets[d] = (tau / chord) * (tau / chord);
                            if (tFull[d] < 1) opaque = true;
                        }
                        if (!opaque) continue;  // every ray passed (cut-out surfaces): empty
                        Sym3 S;
                        if (!solve6(rows, targets, S.m)) continue;
                        const double top = makePsd(S);
                        if (!(top > 0)) continue;
                        const uint8_t q = brickEncodeDepth((float)(voxel * std::sqrt(top)));
                        if (q == 0) continue;
                        Sym3 shape;
                        for (int e = 0; e < 6; ++e) shape.m[e] = S.m[e] / top;
                        uint8_t code[6];
                        encodeShape(shape, code);
                        b.density[vIndex] = q;
                        std::memcpy(&b.shape[(size_t)vIndex * 6], code, 6);
                        b.occupancy |= 1ull << ((vx / 4) + 4 * ((vy / 4) + 4 * (vz / 4)));
                        // Residuals against what a reader decodes (quantised depth and shape).
                        VoxelFit f;
                        f.index = vIndex;
                        f.tau = brickDecodeDepth(q);
                        f.shape = decodeShape(code);
                        std::memcpy(f.t, tFull, sizeof tFull);
                        float r = 0, rh = 0;
                        for (uint32_t d = 0; d < kDirections; ++d)
                        {
                            const float3 w = dirs[d];
                            const double sigma = f.tau / voxel * std::sqrt(std::max(f.shape.quad(w), 0.0));
                            const double chord = voxel / (std::fabs(w.x) + std::fabs(w.y) + std::fabs(w.z));
                            r = std::max(r, (float)std::fabs(std::exp(-chord * sigma) - tFull[d]));
                            rh = std::max(rh, (float)std::fabs(std::exp(-0.5 * chord * sigma) - tHalf[d]));
                        }
                        b.r.push_back(r);
                        b.rHalf.push_back(rh);
                        fits.push_back(f);
                    }
            if (fits.empty())
            {
                bricks[slot].density.clear();
                return;
            }
            // Brick-mean shape (optical-depth weighted), renormalised; the residual of using it instead of the voxel's.
            Sym3 mean;
            double weight = 0;
            for (const VoxelFit& f : fits)
            {
                for (int e = 0; e < 6; ++e) mean.m[e] += f.tau * f.shape.m[e];
                weight += f.tau;
            }
            double values[3], vectors[3][3];
            eigen(mean, values, vectors);
            const double meanTop = std::max({ values[0], values[1], values[2], 1e-30 });
            for (int e = 0; e < 6; ++e) mean.m[e] /= meanTop;
            for (const VoxelFit& f : fits)
            {
                float rb = 0;
                for (uint32_t d = 0; d < kDirections; ++d)
                {
                    const float3 w = dirs[d];
                    const double sigma = f.tau / voxel * std::sqrt(std::max(mean.quad(w), 0.0));
                    const double chord = voxel / (std::fabs(w.x) + std::fabs(w.y) + std::fabs(w.z));
                    rb = std::max(rb, (float)std::fabs(std::exp(-chord * sigma) - f.t[d]));
                }
                b.rBrick.push_back(rb);
            }
            gpu::BrickHeader& header = b.header;
            header.cell = sx | (sy << 10) | (sz << 20);
            header.shape[0] = toHalf((float)mean.m[0]) | ((uint32_t)toHalf((float)mean.m[1]) << 16);
            header.shape[1] = toHalf((float)mean.m[2]) | ((uint32_t)toHalf((float)mean.m[3]) << 16);
            header.shape[2] = toHalf((float)mean.m[4]) | ((uint32_t)toHalf((float)mean.m[5]) << 16);
            auto byte = [](double v) { return (uint32_t)std::clamp((int)std::lround(v * 255), 0, 255); };
            if (albedoWeight > 0)
                header.albedo = byte(albedo[0] / albedoWeight) | (byte(albedo[1] / albedoWeight) << 8) | (byte(albedo[2] / albedoWeight) << 16);
            header.material = (uint32_t)(std::max_element(materialHits.begin(), materialHits.end()) - materialHits.begin());
            std::vector<float> r = b.r, rh = b.rHalf, rb = b.rBrick;
            header.residual = byte(percentile(r, 0.99)) | (byte(*std::max_element(b.r.begin(), b.r.end())) << 8) | (byte(percentile(rh, 0.99)) << 16) |
                              (byte(percentile(rb, 0.99)) << 24);
            used[slot] = 1;
        });
        for (uint32_t slot = 0; slot < slots; ++slot)
        {
            if (!used[slot]) continue;
            BrickOut& b = bricks[slot];
            const uint32_t index = (uint32_t)out.headers.size();
            out.grid[level.gridOffset + slot] = index;
            b.header.meshLevel = k;  // the brick mesh index is added when meshes are assembled
            out.headers.push_back(b.header);
            out.density.insert(out.density.end(), b.density.begin(), b.density.end());
            out.shape.insert(out.shape.end(), b.shape.begin(), b.shape.end());
            out.occupancy.push_back(b.occupancy);
            out.stats.voxels += (uint32_t)b.r.size();
            allR.insert(allR.end(), b.r.begin(), b.r.end());
            allHalf.insert(allHalf.end(), b.rHalf.begin(), b.rHalf.end());
            allBrick.insert(allBrick.end(), b.rBrick.begin(), b.rBrick.end());
        }
    }
    out.stats.bricks = (uint32_t)out.headers.size();
    auto maxOf = [](const std::vector<float>& v) { return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end()); };
    out.stats.rMax = maxOf(allR);
    out.stats.rHalfMax = maxOf(allHalf);
    out.stats.rBrickMax = maxOf(allBrick);
    out.stats.rP99 = percentile(allR, 0.99);
    out.stats.rHalfP99 = percentile(allHalf, 0.99);
    out.stats.rBrickP99 = percentile(allBrick, 0.99);
    out.stats.bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

// ---- cache: one file per mesh, keyed by the SHA-256 of everything the bake reads.

std::string cacheKey(const scene::Scene& scene, uint32_t meshIndex, const BrickSettings& settings, float width)
{
    const scene::Mesh& m = scene.meshes[meshIndex];
    Sha256 h;
    const uint32_t header[4] = { kCacheVersion, settings.brickEdge, settings.bakeRays, 0 };
    h.update(header, sizeof header);
    h.update(&width, sizeof width);
    h.update(m.positions.data(), m.positions.size() * sizeof(float3));
    h.update(m.uv0.data(), m.uv0.size() * sizeof(float2));
    h.update(m.indices.data(), m.indices.size() * sizeof(uint32_t));
    for (const scene::Submesh& sm : m.submeshes)
    {
        h.update(&sm, sizeof sm);
        const scene::Material& mat = scene.materials[sm.material];
        h.update(&mat.baseColor, sizeof mat.baseColor);
        h.update(&mat.alphaCutoff, sizeof mat.alphaCutoff);
        if (mat.baseColorTexture != scene::kNone && mat.baseColorTexture < scene.textures.size())
        {
            const scene::Texture& t = scene.textures[mat.baseColorTexture];
            const uint32_t info[4] = { t.width, t.height, (uint32_t)t.format, t.wrap ? 1u : 0u };
            h.update(info, sizeof info);
            h.update(t.texels.data(), t.texels.size());
        }
    }
    const std::array<uint8_t, 32> d = h.finish();
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d)
    {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}

template <typename T>
void put(std::vector<uint8_t>& out, const std::vector<T>& v)
{
    const uint64_t n = v.size();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
    out.insert(out.end(), p, p + 8);
    const uint8_t* q = reinterpret_cast<const uint8_t*>(v.data());
    out.insert(out.end(), q, q + n * sizeof(T));
}

template <typename T>
bool get(const std::vector<uint8_t>& in, size_t& at, std::vector<T>& v)
{
    if (at + 8 > in.size()) return false;
    uint64_t n;
    std::memcpy(&n, &in[at], 8);
    at += 8;
    if (n > (in.size() - at) / sizeof(T)) return false;
    v.resize((size_t)n);
    std::memcpy(v.data(), &in[at], (size_t)n * sizeof(T));
    at += (size_t)n * sizeof(T);
    return true;
}

void saveCache(const std::filesystem::path& file, const MeshBake& b)
{
    std::vector<uint8_t> out;
    put(out, std::vector<gpu::BrickMesh>{ b.mesh });
    put(out, b.levels);
    put(out, b.grid);
    put(out, b.headers);
    put(out, b.density);
    put(out, b.shape);
    put(out, b.occupancy);
    put(out, std::vector<BrickMeshStats>{ b.stats });
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
        if (!f) return;  // a cache that cannot be written only costs the next run a bake
    }
    std::filesystem::rename(tmp, file, ec);
}

bool loadCache(const std::filesystem::path& file, MeshBake& b)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    const std::vector<uint8_t> in = readBinaryFile(file);
    size_t at = 0;
    std::vector<gpu::BrickMesh> mesh;
    std::vector<BrickMeshStats> stats;
    if (!get(in, at, mesh) || mesh.size() != 1 || !get(in, at, b.levels) || !get(in, at, b.grid) || !get(in, at, b.headers) || !get(in, at, b.density) ||
        !get(in, at, b.shape) || !get(in, at, b.occupancy) || !get(in, at, stats) || stats.size() != 1 || at != in.size())
        return false;
    b.mesh = mesh[0];
    b.stats = stats[0];
    b.stats.cached = true;
    return true;
}
} // namespace

float brickDecodeDepth(uint8_t q) { return q == 0 ? 0.0f : 8.0f * std::exp2((q - 255.0f) / 24.0f); }

uint8_t brickEncodeDepth(float tau)
{
    if (!(tau > 0)) return 0;
    const float q = 255.0f + 24.0f * std::log2(tau / 8.0f);
    if (q < 0.5f) return 0;  // optical depth below 0.0053 x 2^(-1/48): transmittance above 0.995, drawn as empty
    return (uint8_t)std::clamp((int)std::lround(q), 1, 255);
}

BrickSettings BrickSettings::fromQuality(const QualityConfig& q)
{
    BrickSettings s;
    s.brickEdge = (uint32_t)q.integer("visibility.brick_resolution");
    s.maxFeatureWidth = (float)q.number("visibility.brick_max_feature_width");
    s.bakeRays = (uint32_t)q.integer("visibility.brick_bake_rays");
    s.residualMax = (float)q.number("visibility.brick_residual_max");
    if (s.brickEdge != 16) fail("visibility.brick_resolution must be 16 (the occupancy word covers 4^3 cells of 4^3 voxels)");
    if (s.bakeRays < 1) fail("visibility.brick_bake_rays must be positive");
    return s;
}

BrickData bakeBricks(const scene::Scene& scene, const BrickSettings& settings, const std::string& cacheDirectory, std::vector<BrickMeshStats>* stats)
{
    BrickData data;
    data.meshOf.assign(scene.meshes.size(), kNone);
    if (!(settings.maxFeatureWidth > 0)) return data;
    // Eligible meshes and their thin widths (skinned meshes are not baked: bricks are static shapes).
    std::vector<float> width(scene.meshes.size(), FLT_MAX);
    for (uint32_t i = 0; i < scene.meshes.size(); ++i)
    {
        const scene::Mesh& m = scene.meshes[i];
        if (m.indices.empty() || !m.skin.inverseBind.empty()) continue;
        const detail::MeshWidthContext widths(m.positions, m.indices);
        width[i] = thinWidth(m, widths);
    }
    for (uint32_t i = 0; i < scene.meshes.size(); ++i)
    {
        if (!(width[i] <= settings.maxFeatureWidth)) continue;
        MeshBake b;
        const std::string key = cacheKey(scene, i, settings, width[i]);
        const std::filesystem::path file = cacheDirectory.empty() ? std::filesystem::path() : std::filesystem::path(cacheDirectory) / (key + ".brk");
        if (cacheDirectory.empty() || !loadCache(file, b))
        {
            b = bakeMesh(scene, i, settings, width[i]);
            if (!cacheDirectory.empty() && !b.headers.empty()) saveCache(file, b);
        }
        b.stats.mesh = i;
        if (b.headers.empty()) continue;
        if (settings.residualMax > 0 && b.stats.rP99 > settings.residualMax)
            fail("bricks: mesh '%s' residual P99 %.4f above visibility.brick_residual_max %.4f (max %.4f)", scene.meshes[i].name.c_str(), b.stats.rP99,
                 settings.residualMax, b.stats.rMax);
        const uint32_t meshIndex = (uint32_t)data.meshes.size(), brickBase = (uint32_t)data.headers.size(), gridBase = (uint32_t)data.grid.size();
        data.meshOf[i] = meshIndex;
        b.mesh.firstLevel = (uint32_t)data.levels.size();
        data.meshes.push_back(b.mesh);
        for (gpu::BrickLevel level : b.levels)
        {
            level.gridOffset += gridBase;
            data.levels.push_back(level);
        }
        for (uint32_t slot : b.grid) data.grid.push_back(slot == kNone ? kNone : slot + brickBase);
        for (gpu::BrickHeader h : b.headers)
        {
            h.meshLevel |= meshIndex << 8;
            data.headers.push_back(h);
        }
        data.density.insert(data.density.end(), b.density.begin(), b.density.end());
        data.shape.insert(data.shape.end(), b.shape.begin(), b.shape.end());
        data.occupancy.insert(data.occupancy.end(), b.occupancy.begin(), b.occupancy.end());
        if (stats) stats->push_back(b.stats);
    }
    return data;
}
} // namespace unx::clusterbuilder
