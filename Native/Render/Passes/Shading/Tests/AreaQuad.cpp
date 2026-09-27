// M study and CPU replica (render A, 2026-09-27): area lights for lobes without a closed form - the sheen lobe
// (MATERIAL_LAYERS 1.4) and the anisotropic GGX lobe (1.5) - by one parameterisation of the light's region clipped to the
// shading point's hemisphere, for all four shapes (rect, disk, sphere, tube).
//
// Region: the light's outline as seen from the shading point (AreaLight.hlsli's outlines: rect edges, the disk's ellipse,
// the sphere's subtended circle, the capsule's generators and end-sphere arcs), clipped to z >= 0 (local frame, z = n) and
// closed by the horizon chord. The clipped region is convex. It is fanned from its first vertex A (or, for a whole
// ellipse above the horizon, from the direction of its centre, over the full circle): every boundary element not
// adjacent to A as a straight edge spans an azimuth range psi about A, and along each azimuth the geodesic distance to
// the element is closed form (a great circle: one atan2; an ellipse: the plane of A and the azimuth cuts it at two
// parameters, the exit is the farther). Radial variable s = (1 - cos t) / (1 - cos R(psi)): d omega = (1 - cos R) ds dpsi.
//   sheen      Gauss-Legendre n x n per element (the integrand is smooth once the horizon's kink is on the boundary);
//   aniso      the lobe's own measure: f_s cos d omega = W(l) P22(m) d^2 m with W = F G2 (v.h) / (n.v h.n) and m the slope
//              of h = normalize(v + l); in u = m / alpha, P22 is the unit density 1 / (pi (1 + |u|^2)^2). Each element's
//              (psi, s) square is cut into K x K cells, each cell's corners mapped to u; the cell's mass is exact for the
//              straight-edged quad in u (Green: sum over edges of (u_a x d) / (2 pi) int_0^1 dt / (1 + |u_a + t d|^2), an
//              atan) and weighted by W at the cell centre. Narrow lobes put their mass in few cells exactly.
// References (independent): the sheen region integral by a midpoint grid over the light's bounding cone (inside tests
// by ray casts against the shape); the anisotropic one by a midpoint grid over the lobe's own measure (u by the radial
// CDF, reflected to l, inside tests).
//
// Usage: unx_test_shading_areaquad [--order N] [--cells K] [--grid G]
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"
#include "AreaQuadReplica.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace unx;
namespace model = unx::scene::model;

namespace
{
using namespace unx::aqr;
} // namespace

int main(int argc, char** argv)
{
    int order = 6, cells = 6, grid = 2048;
    for (int i = 1; i < argc; ++i)
    {
        const std::string s = argv[i];
        if (s == "--order" && i + 1 < argc) order = std::stoi(argv[++i]);
        else if (s == "--cells" && i + 1 < argc) cells = std::stoi(argv[++i]);
        else if (s == "--grid" && i + 1 < argc) grid = std::stoi(argv[++i]);
        else if (s == "--arc-span" && i + 1 < argc) g_arcSpan = std::stod(argv[++i]);
        else if (s == "--arc-cells" && i + 1 < argc) g_arcCells = std::stoi(argv[++i]);
        else if (s == "--verbose") g_verbose = true;
        else if (s == "--vertex-apex") g_interiorApex = false;
        else if (s == "--no-sagitta") g_sagitta = false;
        else if (s == "--radial-groups" && i + 1 < argc) g_radialGroups = std::stoi(argv[++i]);
    }
    (void)order;
    // placements: 4 shapes x directions (incl. across the horizon) x sizes x view angles
    struct Case
    {
        Light L;
        V3 v;
        double r;       // sheen roughness / aniso roughness
        double s;       // aniso strength
        double rot;     // aniso rotation
    };
    std::vector<Case> cases;
    const double thetas[] = { 0.3, 0.9, 1.35, 1.55, 1.7 };  // light direction from the normal (1.7: partly below the horizon)
    const double sizes[] = { 0.05, 0.2, 0.6 };              // angular half size (rad) at distance 1
    const double views[] = { 0.2, 0.9, 1.35 };
    for (int sh = 0; sh < 4; ++sh)
        for (double th : thetas)
            for (double hs : sizes)
                for (double vt : views)
                {
                    Case c;
                    c.L.shape = (Shape)sh;
                    const double phi = 0.7 + sh;
                    c.L.p = V3{ std::sin(th) * std::cos(phi), std::sin(th) * std::sin(phi), std::cos(th) } * 1.0;
                    c.L.forward = normalize(c.L.p * -1 + V3{ 0.2, -0.1, 0.1 });
                    c.L.right = normalize(cross(c.L.forward, V3{ 0.3, 0.9, 0.1 }));
                    const double ext = 2 * std::tan(hs);
                    c.L.sx = sh == Rect ? ext : sh == Tube ? 2.5 * ext : 0.5 * ext;
                    c.L.sy = sh == Rect ? 0.6 * ext : sh == Tube ? 0.15 * ext : 0;
                    if (sh == Tube) c.L.right = normalize(cross(c.L.p, V3{ 0.1, 0.2, 1 }));
                    c.v = V3{ std::sin(vt), 0, std::cos(vt) };
                    cases.push_back(c);
                }
    std::atomic<size_t> next{ 0 };
    const double sheenR[] = { 0.2, 0.5 };
    const double anisoR[][2] = { { 0.3, 0.8 }, { 0.08, 1.0 }, { 0.5, 0.4 } };
    struct Row
    {
        double err[2][3] = {}, ref[2][3] = {};
    };
    std::vector<Row> rows(cases.size());
    auto work = [&]() {
        for (size_t i; (i = next++) < cases.size();)
        {
            const Case& c = cases[i];
            std::vector<Elem> loop, region;
            if (!outline(c.L, loop) || !clipHorizon(loop, region)) continue;
            for (int k = 0; k < 2; ++k)
            {
                const double r = sheenR[k];
                const double q = sheenQuad(region, c.v, r, 6);
                const double ref = coneReference(c.L, grid, [&](V3 l) {
                    return (double)model::evaluateSheenLobe((float)r, { 0, 0, 1 }, { (float)c.v.x, (float)c.v.y, (float)c.v.z }, { (float)l.x, (float)l.y, (float)l.z }) * l.z;
                });
                rows[i].err[0][k] = std::fabs(q - ref), rows[i].ref[0][k] = ref;
            }
            for (int k = 0; k < 3; ++k)
            {
                Aniso a;
                const float2 al = model::anisoAlphas((float)anisoR[k][0], (float)anisoR[k][1]);
                a.at = al.x, a.ab = al.y;
                const double rot = 0.5 + k;
                a.t = V3{ std::cos(rot), std::sin(rot), 0 }, a.b = V3{ -std::sin(rot), std::cos(rot), 0 };
                a.f0 = { 0.9, 0.9, 0.9 };
                const float2 AB = model::anisoSpecularAlbedo({ (float)dot(c.v, a.t), (float)dot(c.v, a.b), (float)c.v.z }, (float)a.at, (float)a.ab);
                a.Ea = AB.x + AB.y;
                const double q = anisoQuad(region, c.v, a, cells);
                const double ref = lobeReference(c.L, c.v, a, grid);
                if (g_verbose && k == 2 && c.L.shape == Tube)
                {
                    // plain Gauss-Legendre of f_s cos (the wide lobe): which of the two is wrong
                    const double gl = glQuad(region, 6, [&](V3 l) {
                        if (l.z <= 0) return 0.0;
                        const V3 h = normalize(c.v + l);
                        const double D = model::distributionGgxAniso((float)dot(h, a.t), (float)dot(h, a.b), (float)h.z, (float)a.at, (float)a.ab);
                        return weight(a, c.v, l) * h.z * D / (4 * dot(c.v, h));  // f_s cos = F G2 D / (4 n.v)
                    });
                    logf("    tube case %zu: cells %.5g, lobe reference %.5g, GL %.5g\n", i, q, ref, gl);
                }
                rows[i].err[1][k] = std::fabs(q - ref), rows[i].ref[1][k] = ref;
            }
        }
    };
    std::vector<std::thread> pool;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    const char* shapeName[] = { "rect", "disk", "sphere", "tube" };
    for (int lobe = 0; lobe < 2; ++lobe)
        for (int k = 0; k < (lobe ? 3 : 2); ++k)
            for (int sh = 0; sh < 4; ++sh)
            {
                double se = 0, sr = 0, worst = 0, peak = 0;
                for (size_t i = 0; i < cases.size(); ++i)
                    if (cases[i].L.shape == sh) peak = std::max(peak, rows[i].ref[lobe][k]);
                int n = 0;
                for (size_t i = 0; i < cases.size(); ++i)
                {
                    if (cases[i].L.shape != sh) continue;
                    se += rows[i].err[lobe][k], sr += rows[i].ref[lobe][k];
                    const double rel = rows[i].err[lobe][k] / std::max(rows[i].ref[lobe][k], 1e-3 * peak);
                    worst = std::max(worst, rel);
                    if (rel > 0.05 && g_verbose)
                        logf("    case %zu (%s, lobe %d/%d): p (%.3f %.3f %.3f) v.z %.3f sx %.3f sy %.3f: quad err %.4g ref %.4g\n", i, shapeName[sh], lobe, k,
                             cases[i].L.p.x, cases[i].L.p.y, cases[i].L.p.z, cases[i].v.z, cases[i].L.sx, cases[i].L.sy, rows[i].err[lobe][k], rows[i].ref[lobe][k]);
                    ++n;
                }
                if (lobe == 0) logf("sheen r %.2f  %-6s: energy-weighted %.5f, worst %.5f (%d placements)\n", sheenR[k], shapeName[sh], sr > 0 ? se / sr : 0, worst, n);
                else logf("aniso r %.2f s %.1f %-6s: energy-weighted %.5f, worst %.5f (%d placements, %d x %d cells)\n", anisoR[k][0], anisoR[k][1], shapeName[sh], sr > 0 ? se / sr : 0, worst, n, cells, cells);
            }
    return 0;
}
