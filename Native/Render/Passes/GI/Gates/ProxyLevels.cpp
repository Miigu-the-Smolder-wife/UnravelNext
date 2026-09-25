// R diagnostic (CPU only, no GPU): the LOD cuts V's cluster builder makes for every skinned mesh of a scene, the input of
// error-bounded character RT proxies (a cut's object-space error against the ray footprint at the character's distance).
// Prints per cut its triangles and error, and the distance beyond which the error is below one pixel's footprint at 4K
// and 1440p (60 deg vertical field of view): d >= error x scale / pixel angle.
// Also measures each cut against the source mesh in the bind pose: the distance from points on the source surface to the
// cut (geometry the cut lost) and from points on the cut to the source (geometry it added), max / P99 / mean over 20,000
// area-weighted points each way, next to the error the cut claims.
//
//   unx_gate_gi_proxylevels --scene <file.unxscene>
// Also R's skin-aware cuts (skinAwareCuts) at several attribute weights, measured the same way.
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/ProxyPoseBound.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#if UNX_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#endif

using namespace unx;
using namespace unx::render;

namespace
{
struct Tri
{
    float3 a, b, c;
};

// Ericson, Real-Time Collision Detection 5.1.5.
float3 closestOnTriangle(float3 p, float3 a, float3 b, float3 c)
{
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const float3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const float3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Uniform grid over a triangle set; nearest distance by growing shells of cells.
struct TriangleGrid
{
    std::vector<Tri> tris;
    float3 lo{}, hi{};
    float cell = 1;
    int n[3] = { 1, 1, 1 };
    std::vector<std::vector<uint32_t>> cells;

    explicit TriangleGrid(std::vector<Tri> t) : tris(std::move(t))
    {
        lo = hi = tris.empty() ? float3{} : tris[0].a;
        for (const Tri& x : tris)
            for (const float3& v : { x.a, x.b, x.c })
            {
                lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
                hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
            }
        const float3 e = hi - lo;
        cell = std::max({ e.x, e.y, e.z, 1e-6f }) / 48;
        for (int k = 0; k < 3; ++k) n[k] = std::max(1, (int)std::ceil((k == 0 ? e.x : k == 1 ? e.y : e.z) / cell) + 1);
        cells.resize((size_t)n[0] * n[1] * n[2]);
        for (uint32_t i = 0; i < (uint32_t)tris.size(); ++i)
        {
            const Tri& x = tris[i];
            int a[3], b[3];
            for (int k = 0; k < 3; ++k)
            {
                auto comp = [&](const float3& v) { return k == 0 ? v.x : k == 1 ? v.y : v.z; };
                const float mn = std::min({ comp(x.a), comp(x.b), comp(x.c) }), mx = std::max({ comp(x.a), comp(x.b), comp(x.c) });
                const float o = k == 0 ? lo.x : k == 1 ? lo.y : lo.z;
                a[k] = std::clamp((int)((mn - o) / cell), 0, n[k] - 1);
                b[k] = std::clamp((int)((mx - o) / cell), 0, n[k] - 1);
            }
            for (int z = a[2]; z <= b[2]; ++z)
                for (int y = a[1]; y <= b[1]; ++y)
                    for (int x0 = a[0]; x0 <= b[0]; ++x0) cells[((size_t)z * n[1] + y) * n[0] + x0].push_back(i);
        }
    }

    float distance(float3 p) const
    {
        int c[3];
        const float q[3] = { p.x - lo.x, p.y - lo.y, p.z - lo.z };
        for (int k = 0; k < 3; ++k) c[k] = std::clamp((int)(q[k] / cell), 0, n[k] - 1);
        float best = 1e30f;  // squared
        const int maxShell = std::max({ n[0], n[1], n[2] });
        for (int r = 0; r <= maxShell; ++r)
        {
            // Every triangle not yet visited lies in cells at least r away from p's cell, so at least (r - 1) cells away.
            if (r > 1 && best < 1e29f && std::sqrt(best) <= (float)(r - 1) * cell) break;
            for (int z = c[2] - r; z <= c[2] + r; ++z)
                for (int y = c[1] - r; y <= c[1] + r; ++y)
                    for (int x = c[0] - r; x <= c[0] + r; ++x)
                    {
                        if (std::max({ std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2]) }) != r) continue;
                        if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1] || z >= n[2]) continue;
                        for (uint32_t i : cells[((size_t)z * n[1] + y) * n[0] + x])
                        {
                            const float3 d = p - closestOnTriangle(p, tris[i].a, tris[i].b, tris[i].c);
                            best = std::min(best, dot(d, d));
                        }
                    }
        }
        return std::sqrt(best);
    }
};

std::vector<float3> samplePoints(const std::vector<Tri>& tris, uint32_t count, uint32_t seed)
{
    std::vector<double> cdf(tris.size());
    double total = 0;
    for (size_t i = 0; i < tris.size(); ++i)
    {
        const float3 c = cross(tris[i].b - tris[i].a, tris[i].c - tris[i].a);
        total += 0.5 * std::sqrt((double)dot(c, c));
        cdf[i] = total;
    }
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<float3> points;
    for (uint32_t k = 0; k < count && total > 0; ++k)
    {
        const size_t i = std::min<size_t>(std::lower_bound(cdf.begin(), cdf.end(), u(rng) * total) - cdf.begin(), tris.size() - 1);
        float r1 = (float)u(rng), r2 = (float)u(rng);
        if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
        points.push_back(tris[i].a + (tris[i].b - tris[i].a) * r1 + (tris[i].c - tris[i].a) * r2);
    }
    return points;
}

struct Stats
{
    float max = 0, p99 = 0, mean = 0;
};

Stats distances(const std::vector<float3>& points, const TriangleGrid& target)
{
    std::vector<float> d;
    d.reserve(points.size());
    double sum = 0;
    for (const float3& p : points)
    {
        d.push_back(target.distance(p));
        sum += d.back();
    }
    std::sort(d.begin(), d.end());
    Stats s;
    if (d.empty()) return s;
    s.max = d.back();
    s.p99 = d[d.size() * 99 / 100];
    s.mean = (float)(sum / d.size());
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--scene" && i + 1 < argc) scenePath = argv[++i];
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required");
#if UNX_HAS_CLUSTERBUILDER
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        const scene::Scene s = scene::load(scenePath);
        const ClusterData cd = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        const double tanHalf = std::tan(60.0 * 3.14159265358979 / 360.0);
        const double pixel4K = 2 * tanHalf / 2160;
        std::vector<bool> skinned(s.meshes.size(), false);
        std::vector<uint32_t> users(s.meshes.size(), 0);
        for (const scene::Instance& in : s.instances)
            if (in.mesh < s.meshes.size())
            {
                ++users[in.mesh];
                if (in.flags & scene::InstanceSkinned) skinned[in.mesh] = true;
            }
        for (uint32_t m = 0; m < (uint32_t)s.meshes.size(); ++m)
        {
            if (!skinned[m] || m >= cd.meshes.size()) continue;
            const auto& range = cd.meshes[m];
            logf("mesh %u '%s': %zu source triangles, %u skinned instances, %u cuts\n", m, s.meshes[m].name.c_str(), s.meshes[m].indices.size() / 3, users[m],
                 range.lodLevelCount);
            const scene::Mesh& sm = s.meshes[m];
            std::vector<Tri> source;
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)
                source.push_back({ sm.positions[sm.indices[i]], sm.positions[sm.indices[i + 1]], sm.positions[sm.indices[i + 2]] });
            const TriangleGrid sourceGrid(source);
            const std::vector<float3> sourcePoints = samplePoints(source, 20000, 1);
            // The first skinned instance's stored pose (palette = jointToModel x inverseBind).
            std::vector<float4> palette;
            for (const scene::Instance& in : s.instances)
            {
                if (in.mesh != m || in.skeleton >= s.skeletons.size()) continue;
                const scene::Skeleton& skel = s.skeletons[in.skeleton];
                for (size_t j = 0; j < sm.skin.inverseBind.size() && j < skel.jointToModel.size(); ++j)
                {
                    const float3x4& a = skel.jointToModel[j];
                    const float3x4& b = sm.skin.inverseBind[j];
                    float r[3][4];
                    for (int y = 0; y < 3; ++y)
                        for (int x = 0; x < 4; ++x)
                            r[y][x] = a.m[y][0] * b.m[0][x] + a.m[y][1] * b.m[1][x] + a.m[y][2] * b.m[2][x] + (x == 3 ? a.m[y][3] : 0.0f);
                    for (int y = 0; y < 3; ++y) palette.push_back({ r[y][0], r[y][1], r[y][2], r[y][3] });
                }
                break;
            }
            std::vector<float3> posed(sm.positions.size());
            for (size_t v = 0; v < sm.positions.size(); ++v)
            {
                float3 out{};
                const float3 p = sm.positions[v];
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += sm.skin.weights[4 * v + k];
                for (int k = 0; k < 4; ++k)
                {
                    const float w = sum > 0 ? sm.skin.weights[4 * v + k] / sum : 0;
                    const uint32_t j = sm.skin.joints[4 * v + k];
                    if (w <= 0 || 3 * j + 2 >= palette.size()) continue;
                    const float4 a = palette[3 * j], b = palette[3 * j + 1], c = palette[3 * j + 2];
                    out = out + float3{ a.x * p.x + a.y * p.y + a.z * p.z + a.w, b.x * p.x + b.y * p.y + b.z * p.z + b.w, c.x * p.x + c.y * p.y + c.z * p.z + c.w } * w;
                }
                posed[v] = palette.empty() ? p : out;
            }
            std::vector<Tri> posedSource;
            for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) posedSource.push_back({ posed[sm.indices[t]], posed[sm.indices[t + 1]], posed[sm.indices[t + 2]] });
            const TriangleGrid posedSourceGrid(posedSource);
            const std::vector<float3> posedSourcePoints = samplePoints(posedSource, 20000, 3);
            render::rt::ProxyPoseSkeleton sk = render::rt::proxyPoseSkeleton(sm);
            // One cut (mesh vertex indices): bind-pose Hausdorff both ways, the posed bound and the true posed Hausdorff.
            auto report = [&](const std::vector<uint32_t>& cutIndices) {
                std::vector<Tri> cut, posedCut;
                for (size_t t = 0; t + 2 < cutIndices.size(); t += 3)
                {
                    cut.push_back({ sm.positions[cutIndices[t]], sm.positions[cutIndices[t + 1]], sm.positions[cutIndices[t + 2]] });
                    posedCut.push_back({ posed[cutIndices[t]], posed[cutIndices[t + 1]], posed[cutIndices[t + 2]] });
                }
                const Stats lost = distances(sourcePoints, TriangleGrid(cut)), added = distances(samplePoints(cut, 20000, 2), sourceGrid);
                const render::rt::ProxyPoseCoefficients pc = render::rt::proxyPoseCoefficients(sm, sk, cutIndices);
                render::rt::ProxyPoseTerms terms;
                render::rt::proxyPoseTerms(sk, palette, terms);
                {
                    // The bound's largest cluster and its terms (what dominates it).
                    auto apply = [&](uint32_t j, float3 q) {
                        const float4 a = palette[3 * j], b = palette[3 * j + 1], c = palette[3 * j + 2];
                        return float3{ a.x * q.x + a.y * q.y + a.z * q.z + a.w, b.x * q.x + b.y * q.y + b.z * q.z + b.w, c.x * q.x + c.y * q.y + c.z * q.z + c.w };
                    };
                    float worst = -1;
                    size_t worstCluster = 0;
                    for (size_t k = 0; k < pc.clusters.size(); ++k)
                    {
                        const auto& cl = pc.clusters[k];
                        float sum = 0;
                        for (uint32_t t = cl.first; t < cl.first + cl.count; ++t)
                        {
                            const auto [i, r] = sk.pairs[pc.terms[t].pair];
                            sum += terms.alpha[pc.terms[t].pair] * pc.terms[t].k1 + length(apply(i, cl.centre) - apply(r, cl.centre)) * pc.terms[t].k2;
                        }
                        if (sum > worst) { worst = sum; worstCluster = k; }
                    }
                    if (!pc.clusters.empty())
                    {
                        const auto& cl = pc.clusters[worstCluster];
                        logf("          %zu clusters, %zu terms; worst cluster at (%.3f %.3f %.3f) sum %.4f:", pc.clusters.size(), pc.terms.size(), cl.centre.x, cl.centre.y,
                             cl.centre.z, worst);
                        for (uint32_t t = cl.first; t < cl.first + cl.count && t < cl.first + 12; ++t)
                        {
                            const auto [i, r] = sk.pairs[pc.terms[t].pair];
                            logf(" (j%u|r%u K1 %.3f K2 %.3f a %.3f b %.3f)", i, r, pc.terms[t].k1, pc.terms[t].k2, terms.alpha[pc.terms[t].pair],
                                 length(apply(i, cl.centre) - apply(r, cl.centre)));
                        }
                        logf("\n");
                    }
                }
                const Stats plost = distances(posedSourcePoints, TriangleGrid(posedCut)), padded = distances(samplePoints(posedCut, 20000, 4), posedSourceGrid);
                const float posedMax = std::max(plost.max, padded.max);
                logf("          bind pose max %.5f m (P99 %.5f); stored pose: measured %.5f m (P99 %.5f), bound %.5f m -> a 4K pixel beyond %.1f m\n",
                     std::max(lost.max, added.max), std::max(lost.p99, added.p99), posedMax, std::max(plost.p99, padded.p99), render::rt::proxyPoseError(pc, sk, terms),
                     posedMax / pixel4K);
            };
            for (uint32_t l = range.lodLevelOffset; l < range.lodLevelOffset + range.lodLevelCount; ++l)
            {
                const gpu::LodLevel& level = cd.lodLevels[l];
                logf("  V cut %2u: %7u triangles, claimed error %.5f m\n", l - range.lodLevelOffset, level.triangleCount, level.error);
                std::vector<uint32_t> cutIndices;
                for (uint32_t k = level.clusterOffset; k < level.clusterOffset + level.clusterCount; ++k)
                {
                    const gpu::Cluster& c = cd.clusters[cd.lodLevelClusters[k]];
                    for (uint32_t t = 0; t < ((c.counts >> 8) & 0xFFu); ++t)
                    {
                        const uint32_t packed = cd.clusterTriangles[c.triangleOffset + t];
                        for (uint32_t v : { packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu }) cutIndices.push_back(cd.clusterVertexIndices[c.vertexOffset + v]);
                    }
                }
                report(cutIndices);
            }
            const uint32_t budget = (uint32_t)quality.integer("raytracing.character_proxy_triangles");
            for (const float weight : { 0.0f, 1.0f, 4.0f, 16.0f })
            {
                const auto cuts = render::rt::skinAwareCuts(sm, sk, budget, weight);
                for (size_t l = 0; l < cuts.size(); ++l)
                {
                    std::vector<uint32_t> cutIndices;
                    for (const auto& list : cuts[l]) cutIndices.insert(cutIndices.end(), list.begin(), list.end());
                    logf("  R skin-aware cut %zu (attribute weight %.0f): %zu triangles\n", l, weight, cutIndices.size() / 3);
                    report(cutIndices);
                }
            }
        }
        return 0;
#else
        fail("needs the integrated build (Tools/CI/Build.ps1 -Track all): V's cluster builder is not in this one");
#endif
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
