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
constexpr double kPi = 3.14159265358979323846;
struct V3
{
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) { return a * (1 / len(a)); }

// ---- light outlines (relative to the shading point, local frame)
enum Shape { Rect, Disk, Sphere, Tube };
struct Light
{
    Shape shape;
    V3 p, forward, right;  // centre; rect/disk emit along +forward; tube axis along right
    double sx = 0, sy = 0;  // rect size; disk radius sx; sphere radius sx; tube length sx, radius sy
};

// Boundary element: kind 0 = straight (segment a -> b of 3D points), 1 = arc c + u cos th + w sin th for th from t0 to t1.
struct Elem
{
    int kind = 0;
    V3 a, b;          // endpoints (3D)
    V3 c, u, w;
    double t0 = 0, t1 = 0;
};
V3 arcPoint(const Elem& e, double th) { return e.c + e.u * std::cos(th) + e.w * std::sin(th); }

void subtendedCircle(V3 p, double r, V3& c, V3& u, V3& w)
{
    const double d2 = dot(p, p), k = r * r / d2, rc = r * std::sqrt(1 - k);
    const V3 dir = normalize(p);
    const V3 t1 = normalize(std::fabs(dir.x) < 0.9 ? cross(dir, { 1, 0, 0 }) : cross(dir, { 0, 1, 0 }));
    c = p * (1 - k);
    u = t1 * rc;
    w = cross(dir, t1) * rc;
}
double arcParam(V3 c, V3 u, V3 w, V3 q)  // parameter of a point on the ellipse
{
    const V3 d = q - c;
    return std::atan2(dot(d, w) / dot(w, w), dot(d, u) / dot(u, u));
}

// The outline as a closed loop of elements; false if the light contributes nothing (behind, inside).
bool outline(const Light& L, std::vector<Elem>& out)
{
    out.clear();
    const V3 up = cross(L.forward, L.right);
    if (L.shape == Rect || L.shape == Disk)
    {
        if (dot(L.p * -1, L.forward) <= 0) return false;
        if (L.shape == Rect)
        {
            const V3 ex = L.right * (0.5 * L.sx), ey = up * (0.5 * L.sy);
            const V3 q[4] = { L.p - ex - ey, L.p + ex - ey, L.p + ex + ey, L.p - ex + ey };
            for (int i = 0; i < 4; ++i) out.push_back({ 0, q[i], q[(i + 1) % 4] });
            return true;
        }
        Elem e;
        e.kind = 1;
        e.c = L.p, e.u = L.right * L.sx, e.w = up * L.sx, e.t0 = 0, e.t1 = 2 * kPi;
        e.a = e.b = arcPoint(e, 0);
        out.push_back(e);
        return true;
    }
    const double r = L.shape == Tube ? L.sy : L.sx;
    const V3 axis = L.right * L.sx;
    const V3 a = L.p - axis * 0.5, b = L.p + axis * 0.5;
    const V3 aPerp = a - L.right * dot(a, L.right);
    const double dPerp = len(aPerp);
    if (L.shape == Sphere || dPerp <= r * 1.0001)
    {
        const V3 centre = L.shape == Sphere ? L.p : (dot(a, a) < dot(b, b) ? a : b);
        if (dot(centre, centre) <= r * r) return false;
        Elem e;
        e.kind = 1;
        subtendedCircle(centre, r, e.c, e.u, e.w);
        e.t0 = 0, e.t1 = 2 * kPi;
        e.a = e.b = arcPoint(e, 0);
        out.push_back(e);
        return true;
    }
    if (std::min(dot(a, a), dot(b, b)) <= r * r) return false;
    const V3 ah = aPerp * (1 / dPerp), uh = normalize(cross(L.right, ah));
    const double along = std::sqrt(1 - r * r / (dPerp * dPerp));
    const V3 mp = ah * (-r / dPerp) + uh * along, mm = ah * (-r / dPerp) - uh * along;
    const V3 a0 = a + mp * r, b0 = b + mp * r, b1 = b + mm * r, a1 = a + mm * r;
    auto cap = [&](V3 end, V3 p0, V3 p1, V3 outward) {
        Elem e;
        e.kind = 1;
        subtendedCircle(end, r, e.c, e.u, e.w);
        const V3 ed = normalize(end);
        const V3 mid = e.c + normalize(outward - ed * dot(outward, ed)) * len(e.u);
        const double s0 = arcParam(e.c, e.u, e.w, p0), s1 = arcParam(e.c, e.u, e.w, p1), sm = arcParam(e.c, e.u, e.w, mid);
        const double span = s1 - s0 - 2 * kPi * std::floor((s1 - s0) / (2 * kPi));
        const double toMid = sm - s0 - 2 * kPi * std::floor((sm - s0) / (2 * kPi));
        e.t0 = s0;
        e.t1 = toMid < span ? s0 + span : s0 + span - 2 * kPi;
        e.a = arcPoint(e, e.t0), e.b = arcPoint(e, e.t1);
        return e;
    };
    out.push_back({ 0, a0, b0 });
    out.push_back(cap(b, b0, b1, L.right));
    out.push_back({ 0, b1, a1 });
    out.push_back(cap(a, a1, a0, L.right * -1));
    return true;
}

// Clip the loop to z >= 0 and close it with the horizon chord; the result starts at the exit point when clipped.
bool clipHorizon(const std::vector<Elem>& in, std::vector<Elem>& out)
{
    std::vector<Elem> pieces;
    V3 exitP, entryP;
    bool haveExit = false, haveEntry = false;
    int exitAfter = -1;  // index in pieces after which the horizon chord goes
    for (const Elem& e : in)
    {
        if (e.kind == 0)
        {
            if (e.a.z < 0 && e.b.z < 0) continue;
            Elem s = e;
            if (e.a.z < 0)
            {
                s.a = e.a + (e.b - e.a) * (e.a.z / (e.a.z - e.b.z)), s.a.z = 0;
                entryP = s.a, haveEntry = true;
            }
            else if (e.b.z < 0)
            {
                s.b = e.a + (e.b - e.a) * (e.a.z / (e.a.z - e.b.z)), s.b.z = 0;
                exitP = s.b, haveExit = true;
            }
            pieces.push_back(s);
            if (e.b.z < 0) exitAfter = (int)pieces.size() - 1;
            continue;
        }
        // arc: roots of c.z + u.z cos th + w.z sin th = 0 inside [t0, t1] (either direction)
        const double A = e.u.z, B = e.w.z, R = std::sqrt(A * A + B * B);
        std::vector<double> cuts = { e.t0 };
        if (R > std::fabs(e.c.z))
        {
            const double ph = std::atan2(B, A), g = std::acos(-e.c.z / R);
            for (double root : { ph + g, ph - g })
            {
                // every copy of the root inside the open interval
                const double lo = std::min(e.t0, e.t1), hi = std::max(e.t0, e.t1);
                double x = root - 2 * kPi * std::floor((root - lo) / (2 * kPi));
                for (; x < hi; x += 2 * kPi)
                    if (x > lo) cuts.push_back(x);
            }
        }
        cuts.push_back(e.t1);
        const bool up = e.t1 >= e.t0;
        std::sort(cuts.begin() + 1, cuts.end() - 1, [&](double x, double y) { return up ? x < y : x > y; });
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
        {
            const double m = 0.5 * (cuts[k] + cuts[k + 1]);
            if (arcPoint(e, m).z < 0) continue;
            Elem s = e;
            s.t0 = cuts[k], s.t1 = cuts[k + 1];
            s.a = arcPoint(s, s.t0), s.b = arcPoint(s, s.t1);
            if (k > 0) s.a.z = 0, entryP = s.a, haveEntry = true;
            if (k + 2 < cuts.size()) s.b.z = 0, exitP = s.b, haveExit = true, exitAfter = -2;
            pieces.push_back(s);
            if (exitAfter == -2) exitAfter = (int)pieces.size() - 1;
        }
    }
    out.clear();
    if (pieces.empty()) return false;
    if (!haveExit || !haveEntry)
    {
        out = pieces;
        return true;
    }
    // order: horizon chord (exit -> entry) first, then the pieces after the exit, cyclically
    Elem h;
    h.kind = 0, h.a = exitP, h.b = entryP;
    out.push_back(h);
    const int n = (int)pieces.size();
    for (int k = 1; k <= n; ++k) out.push_back(pieces[(exitAfter + k) % n]);
    return true;
}

// ---- the fan
struct Fan
{
    V3 A, t1, t2;
    bool interior = false;  // apex inside (a whole ellipse): psi over [0, 2 pi)
};
double azimuth(const Fan& f, V3 q) { const V3 d = normalize(q); return std::atan2(dot(d, f.t2), dot(d, f.t1)); }
double wrap(double x) { return x - 2 * kPi * std::floor((x + kPi) / (2 * kPi)); }
// geodesic distance from A along e to the element
double radial(const Fan& f, const Elem& e, V3 dir)
{
    if (e.kind == 0)
    {
        const V3 g = cross(e.a, e.b);
        const double a = dot(f.A, g), b = dot(dir, g), sa = a < 0 ? -1.0 : 1.0;
        return std::atan2(a * sa, -b * sa);
    }
    const V3 k = cross(f.A, dir);
    const double p = dot(e.u, k), q = dot(e.w, k), c0 = dot(e.c, k), R = std::sqrt(p * p + q * q);
    const double ph = std::atan2(q, p), g = std::acos(std::clamp(-c0 / R, -1.0, 1.0));
    double best = -10;
    for (double th : { ph + g, ph - g })
    {
        const V3 P = normalize(arcPoint(e, th));
        best = std::max(best, std::atan2(dot(P, dir), dot(P, f.A)));
    }
    return best;
}
// tangent direction (unit, at A) of an arc element that starts (atStart) or ends at A, pointing into the arc
V3 arcTangentAtApex(const Fan& f, const Elem& e, bool atStart)
{
    const double th = atStart ? e.t0 : e.t1, sgn = (e.t1 >= e.t0 ? 1.0 : -1.0) * (atStart ? 1.0 : -1.0);
    V3 d = (e.u * -std::sin(th) + e.w * std::cos(th)) * sgn;
    d = d - f.A * dot(d, f.A);
    return normalize(d);
}

double g_arcSpan = 0.7854;  // --arc-span: widest psi interval of one Gauss-Legendre group on a curved piece
int g_arcCells = 24;
int g_radialGroups = 1;  // --radial-groups (study)
bool g_sagitta = true;  // --no-sagitta (study)
bool g_interiorApex = true;  // --vertex-apex (study): fan outlines with arcs from a vertex
bool g_verbose = false;  // --verbose: the placements over 5 %  // --arc-cells: psi cells per 2 pi on a curved piece (aniso)

template <class Cell>
double integrate(const std::vector<Elem>& region, Cell cell)
{
    Fan f;
    const bool whole = region.size() == 1 && region[0].kind == 1 && std::fabs(std::fabs(region[0].t1 - region[0].t0) - 2 * kPi) < 1e-9;
    if (whole) f.A = normalize(region[0].c), f.interior = true;
    else f.A = normalize(region[0].a);
    const V3 ref = std::fabs(f.A.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
    f.t1 = normalize(ref - f.A * dot(ref, f.A));
    f.t2 = cross(f.A, f.t1);
    double sum = 0;
    if (f.interior) return std::fabs(cell(f, region[0], 0.0, 2 * kPi, true));
    bool curved = false;
    for (const Elem& e : region) curved = curved || e.kind == 1;
    if (curved && g_interiorApex)
    {
        // an outline with arcs: the fan from an interior direction (the mean of the boundary's points), every element a
        // piece, each arc in two halves (a piece spans less than pi of azimuth); no slivers, no tangent azimuths
        V3 c{ 0, 0, 0 };
        for (const Elem& e : region)
        {
            c = c + normalize(e.a) + normalize(e.b);
            if (e.kind == 1) c = c + normalize(arcPoint(e, 0.5 * (e.t0 + e.t1))) * 2;
        }
        f.A = normalize(c);
        const V3 r2 = std::fabs(f.A.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
        f.t1 = normalize(r2 - f.A * dot(r2, f.A));
        f.t2 = cross(f.A, f.t1);
        double total = 0;
        for (const Elem& e : region)
        {
            const int halves = e.kind == 1 ? 2 : 1;
            for (int hh = 0; hh < halves; ++hh)
            {
                Elem piece = e;
                if (e.kind == 1)
                {
                    piece.t0 = e.t0 + (e.t1 - e.t0) * hh / 2, piece.t1 = e.t0 + (e.t1 - e.t0) * (hh + 1) / 2;
                    piece.a = arcPoint(piece, piece.t0), piece.b = arcPoint(piece, piece.t1);
                    if (hh == 0) piece.a = e.a;
                    if (hh == 1) piece.b = e.b;
                }
                const double q0 = azimuth(f, piece.a), d = wrap(azimuth(f, piece.b) - q0);
                if (std::fabs(d) < 1e-12) continue;
                total += cell(f, piece, q0, q0 + d, false);
            }
        }
        return std::fabs(total);
    }
    const size_t m = region.size();
    for (size_t i = 0; i < m; ++i)
    {
        const Elem& e = region[i];
        const bool first = i == 0, last = i + 1 == m;
        if (e.kind == 0 && (first || last)) continue;
        double p0, p1;
        if (first) p0 = azimuth(f, f.A + arcTangentAtApex(f, e, true) * 1e-3), p1 = azimuth(f, e.b);
        else if (last) p0 = azimuth(f, e.a), p1 = azimuth(f, f.A + arcTangentAtApex(f, e, false) * 1e-3);
        else p0 = azimuth(f, e.a), p1 = azimuth(f, e.b);
        const double d = wrap(p1 - p0);
        if (std::fabs(d) < 1e-12) continue;
        sum += cell(f, e, p0, p0 + d, false);
    }
    return std::fabs(sum);
}

const double kGL6x[] = { -0.9324695142031521, -0.6612093864662645, -0.2386191860831969, 0.2386191860831969, 0.6612093864662645, 0.9324695142031521 };
const double kGL6w[] = { 0.1713244923791704, 0.3607615730481386, 0.4679139345726910, 0.4679139345726910, 0.3607615730481386, 0.1713244923791704 };

// Arvo's area-preserving map of the spherical triangle (A, B, C) ("Stratified sampling of spherical triangles", 1995)
double sphericalAngle(V3 at, V3 b, V3 c)
{
    const V3 n1 = cross(at, b), n2 = cross(at, c);
    const double l1 = len(n1), l2 = len(n2);
    if (l1 < 1e-15 || l2 < 1e-15) return 0;
    return std::acos(std::clamp(dot(n1, n2) / (l1 * l2), -1.0, 1.0));
}
V3 arvoPoint(V3 A, V3 B, V3 C, double alpha, double area, double cosC, double u1, double u2)
{
    const double ah = u1 * area, sn = std::sin(ah - alpha), cs = std::cos(ah - alpha);
    const double u = cs - std::cos(alpha), v = sn + std::sin(alpha) * cosC;
    const double q = std::clamp(((v * cs - u * sn) * std::cos(alpha) - v) / ((v * sn + u * cs) * std::sin(alpha)), -1.0, 1.0);
    const V3 perpC = normalize(C - A * dot(C, A));
    const V3 Ch = A * q + perpC * std::sqrt(std::max(0.0, 1 - q * q));
    const double z = 1 - u2 * (1 - dot(Ch, B));
    const V3 d = Ch - B * dot(Ch, B);
    const double dl = len(d);
    return dl > 1e-15 ? normalize(B * z + d * (std::sqrt(std::max(0.0, 1 - z * z)) / dl)) : B;
}

// sheen: straight pieces as spherical triangles (apex, edge) by Arvo's map x Gauss-Legendre n x n; curved pieces by the
// polar fan x Gauss-Legendre n x n (the interior fan: trapezoid in psi with 2n points)
template <class Fn>
double glQuad(const std::vector<Elem>& region, int n, Fn f)
{
    return integrate(region, [&](const Fan& F, const Elem& e, double p0, double p1, bool periodic) {
        double s = 0;
        if (e.kind == 0)
        {
            const V3 A = F.A, B = normalize(e.a), C = normalize(e.b);
            const double al = sphericalAngle(A, B, C), be = sphericalAngle(B, C, A), ga = sphericalAngle(C, A, B), area = al + be + ga - kPi;
            if (!(area > 1e-12)) return 0.0;
            for (int j = 0; j < n; ++j)
                for (int k = 0; k < n; ++k)
                    s += 0.25 * kGL6w[j] * kGL6w[k] * f(arvoPoint(A, B, C, al, area, dot(A, B), 0.5 * (kGL6x[j] + 1), 0.5 * (kGL6x[k] + 1)));
            return s * area * (p1 >= p0 ? 1.0 : -1.0);  // (the fan sums signed pieces)
        }
        // composite in psi: sub-intervals of at most g_arcSpan (a large curved region's integrand varies over its span);
        // the whole circle by the trapezoid rule (periodic)
        const int groups = periodic ? 1 : std::max(1, (int)std::ceil(std::fabs(p1 - p0) / g_arcSpan));
        const int np = periodic ? std::max(2 * n, (int)std::ceil(2 * kPi / g_arcSpan) * n) : n * groups;
        for (int j = 0; j < np; ++j)
        {
            const int gi = periodic ? 0 : j / n, gj = periodic ? j : j % n;
            const double g0 = p0 + (p1 - p0) * gi / groups, g1 = p0 + (p1 - p0) * (gi + 1) / groups;
            const double psi = periodic ? p0 + (j + 0.5) * (p1 - p0) / np : g0 + (g1 - g0) * 0.5 * (kGL6x[gj] + 1);
            const double wp = periodic ? (p1 - p0) / np : 0.5 * (g1 - g0) * kGL6w[gj];
            const V3 dir = F.t1 * std::cos(psi) + F.t2 * std::sin(psi);
            const double R = radial(F, e, dir), q = 1 - std::cos(R);
            double inner = 0;
            for (int rg = 0; rg < g_radialGroups; ++rg)
                for (int k = 0; k < n; ++k)
                {
                    const double sr = (rg + 0.5 * (kGL6x[k] + 1)) / g_radialGroups;
                    const double ct = 1 - sr * q, st = std::sqrt(std::max(0.0, 1 - ct * ct));
                    inner += 0.5 * kGL6w[k] / g_radialGroups * f(F.A * ct + dir * st);
                }
            s += wp * q * inner;
        }
        return s;
    });
}

double sheenQuad(const std::vector<Elem>& region, V3 v, double r, int n)
{
    return glQuad(region, n, [&](V3 l) { return (double)model::evaluateSheenLobe((float)r, { 0, 0, 1 }, { (float)v.x, (float)v.y, (float)v.z }, { (float)l.x, (float)l.y, (float)l.z }) * l.z; });
}

// ---- anisotropic lobe
struct Aniso
{
    V3 t, b;          // local lobe axes (n = z)
    double at, ab;    // alphas
    V3 f0;            // (grey here: f0.x used)
    double Ea;        // A_a + B_a of v
};
double lambda(const Aniso& a, V3 w)  // Smith Lambda in the local frame
{
    const double x = dot(w, a.t) * a.at, y = dot(w, a.b) * a.ab;
    return 0.5 * (-1 + std::sqrt(1 + (x * x + y * y) / (w.z * w.z)));
}
double weight(const Aniso& a, V3 v, V3 l)  // F G2 (v.h) / (n.v h.n) x compensation (grey f0)
{
    if (l.z <= 0) return 0;
    const V3 h = normalize(v + l);
    const double vh = dot(v, h), F = a.f0.x + (1 - a.f0.x) * std::pow(1 - std::clamp(vh, 0.0, 1.0), 5);
    const double G2 = 1 / (1 + lambda(a, v) + lambda(a, l));
    return F * G2 * vh / (v.z * h.z) * (1 + a.f0.x * (1 / a.Ea - 1));
}
double uOf2(const Aniso& a, V3 v, V3 l, double& uy)  // u = m / alpha of h = normalize(v + l)
{
    const V3 h = normalize(v + l);
    uy = -dot(h, a.b) / (h.z * a.ab);
    return -dot(h, a.t) / (h.z * a.at);
}
double edgeMass(double ax, double ay, double bx, double by)  // (u_a x d) / (2 pi) int_0^1 dt / (1 + |u_a + t d|^2)
{
    const double dx = bx - ax, dy = by - ay, cr = ax * dy - ay * dx;
    const double q2 = dx * dx + dy * dy, q1 = 2 * (ax * dx + ay * dy);  // (q0 = 1 + |u_a|^2: the discriminant is 4 (q2 + cr^2))
    if (q2 < 1e-30) return 0;
    const double D = std::sqrt(4 * (q2 + cr * cr));
    return cr / (2 * kPi) * (2 / D) * (std::atan((2 * q2 + q1) / D) - std::atan(q1 / D));
}
double anisoQuad(const std::vector<Elem>& region, V3 v, const Aniso& a, int K)
{
    return integrate(region, [&](const Fan& F, const Elem& e, double p0, double p1, bool periodic) {
        (void)periodic;
        // the piece's parameterisation (x across, y from the apex to the boundary; y = 1 is the region's boundary):
        // straight pieces by Arvo's map of the triangle (B, apex, C) (degenerate at the apex, u2 = 1 the edge BC), curved
        // ones by the polar fan
        const V3 B = normalize(e.a), C = normalize(e.b);
        const double aB = sphericalAngle(B, F.A, C), areaT = aB + sphericalAngle(F.A, C, B) + sphericalAngle(C, B, F.A) - kPi, cB = dot(B, F.A);
        if (e.kind == 0 && !(areaT > 1e-12)) return 0.0;
        auto point = [&](double x, double y) {
            if (e.kind == 0) return arvoPoint(B, F.A, C, aB, areaT, cB, x, y);
            const double psi = p0 + (p1 - p0) * x;
            const V3 dir = F.t1 * std::cos(psi) + F.t2 * std::sin(psi);
            const double R = radial(F, e, dir), ct = 1 - y * (1 - std::cos(R)), st = std::sqrt(std::max(0.0, 1 - ct * ct));
            return F.A * ct + dir * st;
        };
        // cells: K each way, more for long pieces (g_arcCells per 2 pi of the piece's angular extent)
        const double across = e.kind == 0 ? std::acos(std::clamp(dot(B, C), -1.0, 1.0)) : std::fabs(p1 - p0);
        const double along = e.kind == 0 ? std::max(std::acos(std::clamp(dot(F.A, B), -1.0, 1.0)), std::acos(std::clamp(dot(F.A, C), -1.0, 1.0)))
                                         : std::acos(std::clamp(dot(F.A, normalize(point(0.5, 1))), -1.0, 1.0));
        const int KP = std::max(K, (int)std::ceil(across / (2 * kPi) * g_arcCells));
        const int KS = std::max(K, (int)std::ceil(along / (2 * kPi) * g_arcCells));
        const int K1 = KS + 1;
        std::vector<double> ux((KP + 1) * K1), uy((KP + 1) * K1);
        for (int j = 0; j <= KP; ++j)
            for (int k = 0; k <= KS; ++k) ux[j * K1 + k] = uOf2(a, v, point((double)j / KP, (double)k / KS), uy[j * K1 + k]);
        double sum = 0;
        for (int j = 0; j < KP; ++j)
            for (int k = 0; k < KS; ++k)
            {
                const int i00 = j * K1 + k, i10 = i00 + K1, i11 = i10 + 1, i01 = i00 + 1;
                const double mass = edgeMass(ux[i00], uy[i00], ux[i10], uy[i10]) + edgeMass(ux[i10], uy[i10], ux[i11], uy[i11]) +
                                    edgeMass(ux[i11], uy[i11], ux[i01], uy[i01]) + edgeMass(ux[i01], uy[i01], ux[i00], uy[i00]);
                // the outer edge lies on the region's boundary: its chord's sagitta in u (the boundary point in the middle)
                // adds P x (2/3) |chord| x sagitta (the parabolic segment), with the cell's orientation
                double m2 = mass;
                if (k == KS - 1 && g_sagitta)
                {
                    double my;
                    const double mx = uOf2(a, v, point((j + 0.5) / KP, 1), my);
                    const double ax = ux[i01], ay = uy[i01], bx = ux[i11], by = uy[i11];  // outer corners (y = 1)
                    const double cx = bx - ax, cy = by - ay, cl = std::sqrt(cx * cx + cy * cy);
                    if (cl > 1e-15)
                    {
                        const double sag = ((mx - 0.5 * (ax + bx)) * cy - (my - 0.5 * (ay + by)) * cx) / cl;  // signed, right of a -> b
                        const double r2 = mx * mx + my * my, P = 1 / (kPi * (1 + r2) * (1 + r2));
                        const double ix = 0.5 * (ux[i00] + ux[i10]) - 0.5 * (ax + bx), iy = 0.5 * (uy[i00] + uy[i10]) - 0.5 * (ay + by);
                        const double inner = (ix * cy - iy * cx) / cl;  // the inner corners' side
                        m2 += (mass < 0 ? -1.0 : 1.0) * P * (2.0 / 3.0) * cl * std::fabs(sag) * ((sag > 0) == (inner > 0) ? -1.0 : 1.0);
                    }
                }
                sum += m2 * weight(a, v, point((j + 0.5) / KP, (k + 0.5) / KS));
            }
        // a straight piece's sign follows the fan's azimuth order like the polar pieces (the Arvo map's own orientation is
        // fixed by B, apex, C; the fan's psi runs from B to C)
        return sum;
    });
}

// ---- references
bool inside(const Light& L, V3 l)
{
    const V3 up = cross(L.forward, L.right);
    if (L.shape == Rect || L.shape == Disk)
    {
        const double den = dot(l, L.forward);
        if (den >= 0) return false;
        const double t = dot(L.p, L.forward) / den;
        if (t <= 0) return false;
        const V3 q = l * t - L.p;
        if (L.shape == Rect) return std::fabs(dot(q, L.right)) <= 0.5 * L.sx && std::fabs(dot(q, up)) <= 0.5 * L.sy;
        return dot(q, q) <= L.sx * L.sx;
    }
    const double r = L.shape == Tube ? L.sy : L.sx;
    V3 a = L.p, b = L.p;
    if (L.shape == Tube) a = L.p - L.right * (0.5 * L.sx), b = L.p + L.right * (0.5 * L.sx);
    // closest approach of the ray (0, l) to the segment [a, b]
    const V3 d = b - a;
    const double dd = dot(d, d);
    double best = 1e30;
    auto segPoint = [&](double s) { return a + d * s; };
    // minimise |l t - (a + d s)| over t >= 0, s in [0, 1]: sample-free closed form via the 2x2 system, clamped
    const double ll = 1, ld = dot(l, d), la = dot(l, a), da = dot(d, a);
    double s = 0, t = 0;
    const double den = ll * dd - ld * ld;
    if (dd > 0 && den > 1e-15) s = std::clamp((ld * la - ll * da) / den, 0.0, 1.0);
    else if (dd > 0) s = std::clamp(-da / dd, 0.0, 1.0);
    for (int it = 0; it < 3; ++it)
    {
        t = std::max(0.0, dot(l, segPoint(s)));
        if (dd > 0) s = std::clamp(dot(l * t - a, d) / dd, 0.0, 1.0);
    }
    const V3 q = l * t - segPoint(s);
    best = dot(q, q);
    return best <= r * r;
}
double boundRadius(const Light& L)
{
    return L.shape == Rect ? 0.5 * std::sqrt(L.sx * L.sx + L.sy * L.sy) : (L.shape == Tube ? 0.5 * L.sx + L.sy : L.sx);
}
// midpoint grid over the bounding cone of the light (half angle beta), clipped to z > 0
template <class F>
double coneReference(const Light& L, int G, F f)
{
    const double d = len(L.p), rb = boundRadius(L);
    const double cb = rb >= d ? -1.0 : std::cos(std::asin(rb / d));
    const V3 ax = normalize(L.p);
    const V3 ref = std::fabs(ax.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
    const V3 e1 = normalize(ref - ax * dot(ref, ax)), e2 = cross(ax, e1);
    double sum = 0;
    for (int i = 0; i < G; ++i)
    {
        const double ct = 1 - (i + 0.5) / G * (1 - cb), st = std::sqrt(std::max(0.0, 1 - ct * ct));
        for (int k = 0; k < G; ++k)
        {
            const double ph = (k + 0.5) * 2 * kPi / G;
            const V3 l = ax * ct + (e1 * std::cos(ph) + e2 * std::sin(ph)) * st;
            if (l.z > 0 && inside(L, l)) sum += f(l);
        }
    }
    return sum * 2 * kPi * (1 - cb) / ((double)G * G);
}
// midpoint grid over the anisotropic lobe's measure (s = rho^2 / (1 + rho^2), phi), reflected to l
double lobeReference(const Light& L, V3 v, const Aniso& a, int G)
{
    double sum = 0;
    for (int i = 0; i < G; ++i)
    {
        const double s = (i + 0.5) / G, rho = std::sqrt(s / (1 - s));
        for (int k = 0; k < G; ++k)
        {
            const double ph = (k + 0.5) * 2 * kPi / G;
            const double mx = rho * std::cos(ph) * a.at, my = rho * std::sin(ph) * a.ab;
            const V3 h = normalize(a.t * -mx + a.b * -my + V3{ 0, 0, 1 });
            const double vh = dot(v, h);
            if (vh <= 0) continue;
            const V3 l = h * (2 * vh) - v;
            if (l.z > 0 && inside(L, l)) sum += weight(a, v, l);
        }
    }
    return sum / ((double)G * G);
}
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
