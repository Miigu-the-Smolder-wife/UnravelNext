// LTC fit of the material model's specular lobe (INTERFACES 8.1 GGX, height-correlated Smith, F = 1) for area lights
// (INTERFACES 8.2: rect, disk, sphere, tube shaded by LTC; the fit error belongs to the quality definition).
// Heitz et al. 2016, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines": for every (view angle,
// roughness) the cosine-weighted lobe f cos / E is approximated by a clamped cosine transformed by M; the shading kernel
// integrates the transformed cosine exactly over the light (AreaLight.hlsli), and the magnitude E with its Fresnel
// split comes from the model's own tables (ShadingCommon.hlsli shSpecularAlbedo), not from this fit.
//   grid: 64 x 64, x = sqrt(1 - NoV) = i / 63 (theta capped at 1.57), y = perceptual roughness j / 63 (alpha = max(r^2,
//         1e-4) as the model); stored M^-1 = [[a, 0, b], [0, 1, 0], [c, 0, d]] in the frame (tangent towards v, bitangent,
//         normal), normalised by its (y, y) entry (the distribution is invariant to M's scale).
//   fit:  Nelder-Mead on M = [X Y Z] [[m11, 0, m13], [0, m22, 0], [0, 0, 1]] and the axis Z (in the plane of incidence,
//         starting at the lobe's mean direction), error
//         sum |f cos - E D_M|^3 / (pdf_brdf + pdf_ltc) over 64 x 64 stratified samples of each (multiple importance).
//         Rows chain along the view angle from normal incidence; normal incidence chains along roughness (isotropic).
//   unx_test_shading_ltcfit            fits and reports the fit error against the committed table (Native/Render/
//                                      Passes/Shading/LtcTable.inl) without writing
//   unx_test_shading_ltcfit --write    fits and rewrites LtcTable.inl   (--threads N: default half the cores)
//   unx_test_shading_ltcfit --sheen    the same for the sheen lobe (MATERIAL_LAYERS 1.4: Charlie D, Smith masking from D,
//                                      f_sh cos with C = 1; A9 area lights): 64 x 32, x as above, rows the sheen table's
//                                      roughness r = 0.1 + 0.9 (j / 31)^2; the second proposal is uniform over the
//                                      hemisphere (the lobe peaks towards the horizon); --write rewrites SheenLtcTable.inl;
//                                      --refine N: N sweeps of neighbour restarts after the chained fit.
// Long runs pause while a GPU timing lock or the user's hold marker exists (UnravelNext .gpulock/current.json, HOLD).
// The error report: per grid point, the L1 distance between the normalised lobe and the LTC (integral of |f cos / E -
// D_M| over the sphere, 0 = exact, 2 = disjoint), worst and mean over the grid.
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <array>
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
constexpr int kN = 64;
constexpr int kSamples = 64;
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
V3 normalize(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }

struct M3  // row-major
{
    double m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    V3 operator*(V3 v) const
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }
    M3 operator*(const M3& b) const
    {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
        return r;
    }
    double det() const
    {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
    M3 inverse() const
    {
        const double d = det();
        M3 r;
        r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
        r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
        r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
        r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
        r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
        r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
        r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
        r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
        r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
        return r;
    }
};

// The model's specular lobe with F = 1 (CPU model, float as the renderer), times the cosine; v, l in the local frame.
double brdfCos(V3 v, V3 l, double alpha)
{
    if (l.z <= 0 || v.z <= 0) return 0;
    const V3 h = normalize(v + l);
    const double sinSq = h.x * h.x + h.y * h.y;
    const double D = model::distributionGgx((float)h.z, (float)sinSq, (float)alpha);
    const double Vis = model::visibilitySmithGgxCorrelated((float)v.z, (float)l.z, (float)alpha);
    return D * Vis * l.z;
}

bool g_sheen = false;  // --sheen: the sheen lobe; 'p' below is then the sheen roughness r, else the GGX alpha

// The fitted lobe times the cosine (F = 1 / C = 1).
double lobeCos(V3 v, V3 l, double p)
{
    if (!g_sheen) return brdfCos(v, l, p);
    if (l.z <= 0 || v.z <= 0) return 0;
    return model::evaluateSheenLobe((float)p, float3{ 0, 0, 1 }, float3{ (float)v.x, (float)v.y, (float)v.z }, float3{ (float)l.x, (float)l.y, (float)l.z }) * l.z;
}

double ggxD(double NoH, double alpha)
{
    const double a2 = alpha * alpha, t = NoH * NoH * (a2 - 1) + 1;
    return a2 / (kPi * t * t);
}

// Visible-normal sampling (Heitz 2018) and its pdf over l.
V3 sampleVndf(V3 v, double alpha, double u1, double u2)
{
    const V3 vh = normalize({ alpha * v.x, alpha * v.y, v.z });
    const double lensq = vh.x * vh.x + vh.y * vh.y;
    const V3 t1 = lensq > 0 ? V3{ -vh.y, vh.x, 0 } * (1 / std::sqrt(lensq)) : V3{ 1, 0, 0 };
    const V3 t2 = cross(vh, t1);
    const double r = std::sqrt(u1), phi = 2 * kPi * u2;
    const double p1 = r * std::cos(phi);
    double p2 = r * std::sin(phi);
    const double s = 0.5 * (1 + vh.z);
    p2 = (1 - s) * std::sqrt(std::max(0.0, 1 - p1 * p1)) + s * p2;
    const V3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0, 1 - p1 * p1 - p2 * p2));
    const V3 ne = normalize({ alpha * nh.x, alpha * nh.y, std::max(0.0, nh.z) });
    return ne * (2 * dot(v, ne)) - v;
}
double pdfVndf(V3 v, V3 l, double alpha)
{
    if (l.z <= 0) return 0;
    const V3 h = normalize(v + l);
    const double tan2 = (1 - v.z * v.z) / (v.z * v.z);
    const double g1 = 2 / (1 + std::sqrt(1 + alpha * alpha * tan2));
    return g1 * ggxD(h.z, alpha) / (4 * v.z);
}

V3 sampleProposal(V3 v, double p, double u1, double u2)
{
    if (!g_sheen) return sampleVndf(v, p, u1, u2);
    const double z = u1, r = std::sqrt(std::max(0.0, 1 - z * z)), phi = 2 * kPi * u2;  // uniform hemisphere
    return { r * std::cos(phi), r * std::sin(phi), z };
}
double pdfProposal(V3 v, V3 l, double p)
{
    if (!g_sheen) return pdfVndf(v, l, p);
    return l.z > 0 ? 1 / (2 * kPi) : 0;
}

struct Ltc
{
    double m11 = 1, m22 = 1, m13 = 0;
    V3 X{ 1, 0, 0 }, Y{ 0, 1, 0 }, Z{ 0, 0, 1 };
    M3 M, invM;
    double detM = 1;
    void update()
    {
        M3 basis;
        basis.m[0][0] = X.x, basis.m[0][1] = Y.x, basis.m[0][2] = Z.x;
        basis.m[1][0] = X.y, basis.m[1][1] = Y.y, basis.m[1][2] = Z.y;
        basis.m[2][0] = X.z, basis.m[2][1] = Y.z, basis.m[2][2] = Z.z;
        M3 p;
        p.m[0][0] = m11, p.m[0][1] = 0, p.m[0][2] = m13;
        p.m[1][0] = 0, p.m[1][1] = m22, p.m[1][2] = 0;
        p.m[2][0] = 0, p.m[2][1] = 0, p.m[2][2] = 1;
        M = basis * p;
        invM = M.inverse();
        detM = std::fabs(M.det());
    }
    double eval(V3 l) const  // normalised distribution D_M(l)
    {
        const V3 lo = invM * l;
        const double len = std::sqrt(dot(lo, lo));
        const double z = lo.z / len;
        return std::max(0.0, z) / kPi / (detM * len * len * len);  // D_o(M^-1 l / |M^-1 l|) |det M^-1| / |M^-1 l|^3
    }
    V3 sample(double u1, double u2) const
    {
        const double theta = std::acos(std::sqrt(u1)), phi = 2 * kPi * u2;
        const V3 lo{ std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta) };
        return normalize(M * lo);
    }
};

double fitError(const Ltc& ltc, V3 v, double alpha, double norm)
{
    double error = 0;
    for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i)
        {
            const double u1 = (i + 0.5) / kSamples, u2 = (j + 0.5) / kSamples;
            {
                const V3 l = ltc.sample(u1, u2);
                const double b = lobeCos(v, l, alpha), d = ltc.eval(l);
                const double e = std::fabs(b - norm * d);
                error += e * e * e / (d + pdfProposal(v, l, alpha));
            }
            {
                const V3 l = sampleProposal(v, alpha, u1, u2);
                if (l.z <= 0) continue;
                const double b = lobeCos(v, l, alpha), d = ltc.eval(l);
                const double e = std::fabs(b - norm * d);
                error += e * e * e / (d + pdfProposal(v, l, alpha));
            }
        }
    return error / (kSamples * kSamples);
}

// Norm (directional albedo, F = 1) and mean direction of the cosine-weighted lobe.
void lobeMoments(V3 v, double alpha, double& norm, V3& mean)
{
    norm = 0;
    mean = { 0, 0, 0 };
    for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i)
        {
            const V3 l = sampleProposal(v, alpha, (i + 0.5) / kSamples, (j + 0.5) / kSamples);
            if (l.z <= 0) continue;
            const double w = lobeCos(v, l, alpha) / pdfProposal(v, l, alpha);
            norm += w;
            mean = mean + l * w;
        }
    norm /= kSamples * kSamples;
    mean.y = 0;
    mean = normalize(mean);
}

// Nelder-Mead (downhill simplex) in 'n' <= 4 dimensions.
using P4 = std::array<double, 4>;
template <typename F>
P4 nelderMead(F f, P4 start, int n, double delta, double tolerance, int iterations)
{
    std::array<P4, 5> s{};
    std::array<double, 5> fs{};
    for (int i = 0; i <= n; ++i)
    {
        s[i] = start;
        if (i > 0) s[i][i - 1] += delta;
        fs[i] = f(s[i]);
    }
    for (int it = 0; it < iterations; ++it)
    {
        int lo = 0, hi = 0, nh = 0;
        for (int i = 0; i <= n; ++i)
        {
            if (fs[i] < fs[lo]) lo = i;
            if (fs[i] > fs[hi]) hi = i;
        }
        nh = lo;
        for (int i = 0; i <= n; ++i)
            if (i != hi && fs[i] > fs[nh]) nh = i;
        if (std::fabs(fs[hi] - fs[lo]) <= tolerance * (std::fabs(fs[hi]) + std::fabs(fs[lo])) + 1e-300) break;
        P4 c{};
        for (int i = 0; i <= n; ++i)
            if (i != hi)
                for (int k = 0; k < n; ++k) c[k] += s[i][k] / n;
        auto along = [&](double t) {
            P4 p = c;
            for (int k = 0; k < n; ++k) p[k] = c[k] + t * (s[hi][k] - c[k]);
            return p;
        };
        const P4 r = along(-1);
        const double fr = f(r);
        if (fr < fs[lo])
        {
            const P4 e = along(-2);
            const double fe = f(e);
            if (fe < fr) s[hi] = e, fs[hi] = fe;
            else s[hi] = r, fs[hi] = fr;
        }
        else if (fr < fs[nh]) s[hi] = r, fs[hi] = fr;
        else
        {
            const bool outside = fr < fs[hi];
            const P4 k = along(outside ? -0.5 : 0.5);
            const double fk = f(k);
            if (fk < (outside ? fr : fs[hi])) s[hi] = k, fs[hi] = fk;
            else
                for (int i = 0; i <= n; ++i)
                    if (i != lo)
                    {
                        for (int q = 0; q < n; ++q) s[i][q] = s[lo][q] + 0.5 * (s[i][q] - s[lo][q]);
                        fs[i] = f(s[i]);
                    }
        }
    }
    int lo = 0;
    for (int i = 0; i <= n; ++i)
        if (fs[i] < fs[lo]) lo = i;
    return s[lo];
}

// Non-isotropic cells can also fit the lobe axis Z: its angle in the plane of incidence as a fourth parameter, starting
// at the lobe's mean direction (Heitz fixes Z there; freeing it lowers the error at grazing views).
void setAxis(Ltc& t, double angle)
{
    t.Z = { std::sin(angle), 0, std::cos(angle) };
    t.Y = { 0, 1, 0 };
    t.X = normalize(cross(t.Y, t.Z));
}

void fitWith(Ltc& ltc, V3 v, double alpha, double norm, bool isotropic, bool freeAxis)
{
    const double axis0 = std::atan2(ltc.Z.x, ltc.Z.z);
    auto apply = [&](Ltc& t, const P4& x) {
        if (isotropic)
        {
            t.m11 = t.m22 = std::fabs(x[0]);
            t.m13 = 0;
        }
        else
        {
            t.m11 = std::fabs(x[0]);
            t.m22 = std::fabs(x[1]);
            t.m13 = x[2];
            setAxis(t, axis0 + x[3]);
        }
        t.update();
    };
    auto objective = [&](const P4& x) {
        Ltc t = ltc;
        if (std::fabs(x[0]) < 1e-7 || (!isotropic && std::fabs(x[1]) < 1e-7)) return 1e300;
        apply(t, x);
        return fitError(t, v, alpha, norm);
    };
    const P4 start = { ltc.m11, ltc.m22, ltc.m13, 0 };
    const P4 x = nelderMead(objective, start, isotropic ? 1 : (freeAxis ? 4 : 3), 0.05, 1e-6, 400);
    apply(ltc, x);
}

// The three-parameter fit about the mean direction (Heitz), then the axis freed from that result; the better of the two
// by the fit error, so freeing the axis never makes a cell worse (it can lose the lobe where the objective is flat, as
// at the mirror limit).
void fit(Ltc& ltc, V3 v, double alpha, double norm, bool isotropic)
{
    fitWith(ltc, v, alpha, norm, isotropic, false);
    if (isotropic) return;
    Ltc freed = ltc;
    fitWith(freed, v, alpha, norm, false, true);
    if (fitError(freed, v, alpha, norm) < fitError(ltc, v, alpha, norm)) ltc = freed;
}

V3 viewAt(int t)
{
    const double x = t / double(kN - 1), ct = 1 - x * x, theta = std::min(1.57, std::acos(ct));
    return { std::sin(theta), 0, std::cos(theta) };
}
double alphaAt(int a) { return model::alphaFromRoughness((float)(a / double(kN - 1))); }
constexpr int kSheenRows = 32;
int rows() { return g_sheen ? kSheenRows : kN; }
double rowValue(int a) { return g_sheen ? a / double(kSheenRows - 1) : a / double(kN - 1); }  // reported roughness coordinate
double paramAt(int a)
{
    if (!g_sheen) return alphaAt(a);
    const double y = a / double(kSheenRows - 1);
    return 0.1 + 0.9 * y * y;
}

void waitWhileHeld()
{
    const std::filesystem::path lock = std::filesystem::path(UNX_SOURCE_DIR) / ".gpulock";
    while (std::filesystem::exists(lock / "current.json") || std::filesystem::exists(lock / "HOLD")) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// L1 distance between the normalised lobe and the LTC over the sphere (MIS estimate).
double l1Distance(const Ltc& ltc, V3 v, double alpha, double norm)
{
    double sum = 0;
    for (int j = 0; j < kSamples; ++j)
        for (int i = 0; i < kSamples; ++i)
        {
            const double u1 = (i + 0.5) / kSamples, u2 = (j + 0.5) / kSamples;
            for (int k = 0; k < 2; ++k)
            {
                const V3 l = k == 0 ? ltc.sample(u1, u2) : sampleProposal(v, alpha, u1, u2);
                if (k == 1 && l.z <= 0) continue;
                const double b = lobeCos(v, l, alpha) / norm, d = ltc.eval(l);
                const double pb = pdfProposal(v, l, alpha), pd = d;
                sum += std::fabs(b - d) / (pb + pd);
            }
        }
    return sum / (kSamples * kSamples);
}

// M^-1 normalised by its (y, y) entry: (a, b, c, d) of [[a, 0, b], [0, 1, 0], [c, 0, d]] in the frame (x towards v).
std::array<float, 4> packInverse(const Ltc& ltc)
{
    const M3& i = ltc.invM;
    const double s = 1 / i.m[1][1];
    return { (float)(i.m[0][0] * s), (float)(i.m[0][2] * s), (float)(i.m[2][0] * s), (float)(i.m[2][2] * s) };
}
Ltc unpackInverse(const std::array<float, 4>& p)
{
    M3 inv;
    inv.m[0][0] = p[0], inv.m[0][1] = 0, inv.m[0][2] = p[1];
    inv.m[1][0] = 0, inv.m[1][1] = 1, inv.m[1][2] = 0;
    inv.m[2][0] = p[2], inv.m[2][1] = 0, inv.m[2][2] = p[3];
    Ltc t;
    t.invM = inv;
    t.M = inv.inverse();
    t.detM = std::fabs(t.M.det());
    return t;
}
} // namespace

// ---- polygon check (--polygon-check): what area lights see of the fit ----
// The LTC integral of a rect light (vertices relative to the shading point, local frame): the clamped cosine's integral
// over M^-1 of the polygon, clipped to z >= 0 (plane through the origin: clipping commutes with normalisation),
// (1 / 2 pi) sum acos(a.b) (a x b / |a x b|).z over the edges.
double ltcPolygon(const Ltc& ltc, const std::array<V3, 4>& quad, V3* formFactor = nullptr)
{
    std::vector<V3> in, out;
    for (const V3& q : quad) in.push_back(ltc.invM * q);
    for (size_t i = 0; i < in.size(); ++i)
    {
        const V3 a = in[i], b = in[(i + 1) % in.size()];
        if (a.z >= 0) out.push_back(a);
        if ((a.z >= 0) != (b.z >= 0)) out.push_back(a + (b - a) * (a.z / (a.z - b.z)));
    }
    if (out.size() < 3) return 0;
    V3 f{};
    for (size_t i = 0; i < out.size(); ++i)
    {
        const V3 a = normalize(out[i]), b = normalize(out[(i + 1) % out.size()]);
        const V3 c = cross(a, b);
        const double len = std::sqrt(dot(c, c));
        if (len < 1e-12) continue;
        f = f + c * (std::acos(std::clamp(dot(a, b), -1.0, 1.0)) / len);
    }
    if (f.z < 0) f = f * -1.0;
    if (formFactor) *formFactor = f;
    return f.z / (2 * kPi);
}

// The normalised lobe's integral over the rect by a dense midpoint rule on the rect (d omega = |n_L . l| dA / d^2).
double lobePolygon(V3 v, double p, double norm, const std::array<V3, 4>& quad, int n)
{
    const V3 e0 = quad[1] - quad[0], e1 = quad[3] - quad[0], nl = cross(e0, e1);
    const double area = std::sqrt(dot(nl, nl));
    const V3 nn = nl * (1 / area);
    double sum = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const V3 x = quad[0] + e0 * ((i + 0.5) / n) + e1 * ((j + 0.5) / n);
            const double d2 = dot(x, x);
            const V3 l = x * (1 / std::sqrt(d2));
            sum += lobeCos(v, l, p) * std::fabs(dot(nn, l)) / d2;
        }
    return sum * area / (double(n) * n) / norm;
}

// A rect of angular half-size h (radians) centred on direction (theta, phi), facing the point, rolled by 'roll'.
std::array<V3, 4> rectAt(double theta, double phi, double h, double roll)
{
    const V3 c{ std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta) };
    const V3 up = std::fabs(c.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
    V3 t = normalize(cross(up, c)), b = cross(c, t);
    const V3 tr = t * std::cos(roll) + b * std::sin(roll), br = b * std::cos(roll) - t * std::sin(roll);
    const double r = std::tan(h);
    return { c + (tr + br) * -r, c + (tr - br) * r, c + (tr + br) * r, c + (br - tr) * r };
}

#include "LtcTable.inl"  // kLtcTable (committed fit; zeros before the first --write)
#if __has_include("SheenLtcTable.inl")
#include "SheenLtcTable.inl"  // kSheenLtcTable
#else
static const float kSheenLtcTable[32 * 64 * 4] = {};
#endif

int main(int argc, char** argv)
{
    try
    {
        bool write = false;
        int refine = 0;
        bool polygonCheck = false;
        unsigned threadCount = std::max(1u, std::thread::hardware_concurrency() / 2);  // other sessions share the machine
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--write") write = true;
            if (std::string(argv[i]) == "--sheen") g_sheen = true;
            if (std::string(argv[i]) == "--polygon-check") polygonCheck = true;
            if (std::string(argv[i]) == "--refine" && i + 1 < argc) refine = std::stoi(argv[++i]);
            if (std::string(argv[i]) == "--threads" && i + 1 < argc) threadCount = (unsigned)std::stoul(argv[++i]);
        }
        std::vector<Ltc> fits(kN * kN);
        std::vector<double> norms(kN * kN);
        // Normal incidence along roughness (isotropic, one parameter), from rough to sharp.
        {
            Ltc ltc;
            for (int a = rows() - 1; a >= 0; --a)
            {
                waitWhileHeld();
                const V3 v = viewAt(0);
                const double alpha = paramAt(a);
                double norm;
                V3 mean;
                lobeMoments(v, alpha, norm, mean);
                ltc.X = { 1, 0, 0 };
                ltc.Y = { 0, 1, 0 };
                ltc.Z = { 0, 0, 1 };
                ltc.update();
                fit(ltc, v, alpha, norm, true);
                fits[a * kN] = ltc;
                norms[a * kN] = norm;
            }
        }
        // Each roughness row along the view angle, rows in parallel.
        std::atomic<int> next{ 0 };
        auto worker = [&]() {
            for (int a; (a = next++) < rows();)
            {
                Ltc ltc = fits[a * kN];
                for (int t = 1; t < kN; ++t)
                {
                    waitWhileHeld();
                    const V3 v = viewAt(t);
                    const double alpha = paramAt(a);
                    double norm;
                    V3 mean;
                    lobeMoments(v, alpha, norm, mean);
                    ltc.Z = mean;
                    ltc.Y = { 0, 1, 0 };
                    ltc.X = normalize(cross(ltc.Y, ltc.Z));
                    ltc.update();
                    fit(ltc, v, alpha, norm, false);
                    fits[a * kN + t] = ltc;
                    norms[a * kN + t] = norm;
                }
            }
        };
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < threadCount; ++i) threads.emplace_back(worker);
        for (auto& th : threads) th.join();

        // Neighbour restarts (--refine N sweeps; the sheen lobe's chained fits fall into local minima): every cell tries the
        // previous sweep's neighbours (roughness and view +-1) as starts, keeps a start that already beats it and re-fits
        // from it; the result replaces the cell only if its fit error is lower. Rows in parallel, reading the last sweep.
        for (int sweep = 0; sweep < refine; ++sweep)
        {
            const std::vector<Ltc> last = fits;
            std::atomic<int> row{ 0 };
            std::atomic<int> improved{ 0 };
            auto refineRow = [&]() {
                for (int a; (a = row++) < rows();)
                    for (int t = 0; t < kN; ++t)
                    {
                        waitWhileHeld();
                        const V3 v = viewAt(t);
                        const double alpha = paramAt(a);
                        const int k = a * kN + t;
                        double best = fitError(fits[k], v, alpha, norms[k]);
                        for (const int dk : { -kN, kN, -1, 1 })
                        {
                            const int na = a + (dk == -kN ? -1 : dk == kN ? 1 : 0), nt = t + (dk == -1 ? -1 : dk == 1 ? 1 : 0);
                            if (na < 0 || na >= rows() || nt < 0 || nt >= kN) continue;
                            Ltc start = last[na * kN + nt];
                            start.update();
                            if (fitError(start, v, alpha, norms[k]) >= best) continue;
                            fit(start, v, alpha, norms[k], t == 0);
                            const double e = fitError(start, v, alpha, norms[k]);
                            if (e < best) best = e, fits[k] = start, ++improved;
                        }
                    }
            };
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < threadCount; ++i) pool.emplace_back(refineRow);
            for (auto& th : pool) th.join();
            logf("refine sweep %d: %d cells improved\n", sweep + 1, improved.load());
        }

        // Fit error of this fit and of the committed table.
        double worstFit = 0, meanFit = 0, worstTable = 0, meanTable = 0;
        int worstA = 0, worstT = 0;
        bool haveTable = false;
        const float* table = g_sheen ? kSheenLtcTable : kLtcTable;
        const size_t tableSize = g_sheen ? std::size(kSheenLtcTable) : std::size(kLtcTable);
        for (size_t i = 0; i < tableSize; ++i) haveTable = haveTable || table[i] != 0;
        const int cells = rows() * kN;
        for (int a = 0; a < rows(); ++a)
            for (int t = 0; t < kN; ++t)
            {
                const int k = a * kN + t;
                const Ltc stored = unpackInverse(packInverse(fits[k]));
                const double e = l1Distance(stored, viewAt(t), paramAt(a), norms[k]);
                meanFit += e / cells;
                if (e > worstFit) worstFit = e, worstA = a, worstT = t;
                if (haveTable)
                {
                    const std::array<float, 4> p = { table[4 * k], table[4 * k + 1], table[4 * k + 2], table[4 * k + 3] };
                    const double et = l1Distance(unpackInverse(p), viewAt(t), paramAt(a), norms[k]);
                    meanTable += et / cells;
                    worstTable = std::max(worstTable, et);
                }
            }
        logf("%s LTC fit (L1 distance of the normalised lobe, 0 = exact): mean %.4f, worst %.4f at roughness %.3f, NoV %.3f\n", g_sheen ? "sheen" : "specular", meanFit,
             worstFit, g_sheen ? paramAt(worstA) : rowValue(worstA), viewAt(worstT).z);
        for (int a : { 4, 16, 32, 48, 63 })
        {
            if (g_sheen) a = a * (kSheenRows - 1) / (kN - 1);
            std::string row;
            for (int t : { 0, 16, 32, 48, 56, 63 })
            {
                char b[32];
                std::snprintf(b, sizeof b, " %.4f", l1Distance(unpackInverse(packInverse(fits[a * kN + t])), viewAt(t), paramAt(a), norms[a * kN + t]));
                row += b;
            }
            logf("  roughness %.3f, x = sqrt(1 - NoV) 0 .. 1:%s\n", g_sheen ? paramAt(a) : rowValue(a), row.c_str());
        }
        if (polygonCheck)
        {
            // Per cell 48 rects (8 directions over the upper hemisphere incl. grazing and behind the view, 3 sizes 5 / 15 / 40
            // degrees half-angle, 2 rolls), error = sum |I_ltc - I_true| / sum I_true over the cell's rects (energy-weighted
            // relative error), I_true by a 96 x 96 midpoint rule; worst and mean over the grid, and the share of cells above 3 %.
            std::vector<double> cellError(rows() * kN, 0.0), cellErrorC(rows() * kN, 0.0), cellErrorF(rows() * kN, 0.0);
            std::atomic<int> row{ 0 };
            auto check = [&]() {
                for (int a; (a = row++) < rows();)
                    for (int t = 0; t < kN; t += 3)
                    {
                        waitWhileHeld();
                        const int k = a * kN + t;
                        const Ltc stored = unpackInverse(packInverse(fits[k]));
                        double diff = 0, total = 0, diffC = 0, diffF = 0;
                        for (int d = 0; d < 8; ++d)
                            for (double h : { 5.0, 15.0, 40.0 })
                                for (double roll : { 0.0, 0.6 })
                                {
                                    const double theta = (d % 4 + 0.5) / 4 * 1.45, phi = d < 4 ? 0.3 : kPi - 0.4;
                                    const auto quad = rectAt(theta, phi, h * kPi / 180, roll);
                                    const double truth = lobePolygon(viewAt(t), paramAt(a), norms[k], quad, 96);
                                    V3 ff;
                                    const double iltc = ltcPolygon(stored, quad, &ff);
                                    diff += std::fabs(iltc - truth);
                                    total += truth;
                                    // ratio corrections: the exact normalised lobe over the LTC at one direction (the light's
                                    // centre, clamped above the horizon; the transformed polygon's form-factor direction mapped back)
                                    auto ratio = [&](V3 w) {
                                        w = normalize(w);
                                        if (w.z < 1e-3) w = normalize(V3{ w.x, w.y, 1e-3 });
                                        const double d = stored.eval(w);
                                        return d > 1e-12 ? lobeCos(viewAt(t), w, paramAt(a)) / norms[k] / d : 1.0;
                                    };
                                    const V3 centre = (quad[0] + quad[2]) * 0.5;
                                    diffC += std::fabs(iltc * ratio(centre) - truth);
                                    diffF += std::fabs(iltc * (dot(ff, ff) > 0 ? ratio(stored.M * ff) : 1.0) - truth);
                                }
                        cellError[k] = total > 0 ? diff / total : 0;
                        cellErrorC[k] = total > 0 ? diffC / total : 0;
                        cellErrorF[k] = total > 0 ? diffF / total : 0;
                    }
            };
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < threadCount; ++i) pool.emplace_back(check);
            for (auto& th : pool) th.join();
            double worst = 0, mean = 0;
            int count = 0, over = 0, wa = 0, wt = 0;
            for (int a = 0; a < rows(); ++a)
                for (int t = 0; t < kN; t += 3)
                {
                    const double e = cellError[a * kN + t];
                    mean += e, ++count, over += e > 0.03;
                    if (e > worst) worst = e, wa = a, wt = t;
                }
            for (int m = 1; m < 3; ++m)
            {
                const std::vector<double>& e = m == 1 ? cellErrorC : cellErrorF;
                double w = 0, sumE = 0;
                int c = 0, o = 0;
                for (int a = 0; a < rows(); ++a)
                    for (int t = 0; t < kN; t += 3) sumE += e[a * kN + t], ++c, o += e[a * kN + t] > 0.03, w = std::max(w, e[a * kN + t]);
                logf("  x lobe / LTC ratio at the %s: mean %.4f, worst %.4f, cells over 3 %%: %d of %d\n", m == 1 ? "light centre" : "form-factor direction", sumE / c, w, o, c);
            }
            logf("polygon check (rect lights, sum |I_ltc - I_true| / sum I_true per cell): mean %.4f, worst %.4f at roughness %.3f NoV %.3f, cells over 3 %%: %d of %d\n",
                 mean / count, worst, g_sheen ? paramAt(wa) : rowValue(wa), viewAt(wt).z, over, count);
        }
        if (haveTable) logf("committed table: mean %.4f, worst %.4f\n", meanTable, worstTable);
        if (write && g_sheen)
        {
            std::string s = "// Generated by unx_test_shading_ltcfit --sheen --write (Tests/LtcFit.cpp). LTC inverse matrices of the sheen lobe\n"
                            "// (MATERIAL_LAYERS 1.4, C = 1, normalised; the magnitude is E_sh from the sheen table): 32 x 64 float4 (a, b, c, d)\n"
                            "// of M^-1 = [[a, 0, b], [0, 1, 0], [c, 0, d]], row-major with r = 0.1 + 0.9 (j / 31)^2 along rows and\n"
                            "// x = sqrt(1 - NoV) = i / 63 along columns.\n";
            char b[64];
            std::snprintf(b, sizeof b, "// Fit: L1 distance mean %.4f, worst %.4f.\n", meanFit, worstFit);
            s += b;
            s += "static const float kSheenLtcTable[32 * 64 * 4] = {\n";
            for (int k = 0; k < cells; ++k)
            {
                const std::array<float, 4> p = packInverse(fits[k]);
                char line[128];
                std::snprintf(line, sizeof line, "    %.8ef, %.8ef, %.8ef, %.8ef,\n", p[0], p[1], p[2], p[3]);
                s += line;
            }
            s += "};\n";
            const std::string path = std::string(UNX_SOURCE_DIR) + "/Native/Render/Passes/Shading/SheenLtcTable.inl";
            writeTextFile(path, s);
            logf("wrote %s\n", path.c_str());
        }
        else if (write)
        {
            std::string s = "// Generated by unx_test_shading_ltcfit --write (Tests/LtcFit.cpp). LTC inverse matrices of the material model's\n"
                            "// specular lobe: 64 x 64 float4 (a, b, c, d) of M^-1 = [[a, 0, b], [0, 1, 0], [c, 0, d]], row-major with roughness\n"
                            "// j / 63 along rows and x = sqrt(1 - NoV) = i / 63 along columns.\n";
            char b[64];
            std::snprintf(b, sizeof b, "// Fit: L1 distance mean %.4f, worst %.4f.\n", meanFit, worstFit);
            s += b;
            s += "static const float kLtcTable[64 * 64 * 4] = {\n";
            for (int k = 0; k < kN * kN; ++k)
            {
                const std::array<float, 4> p = packInverse(fits[k]);
                char line[128];
                std::snprintf(line, sizeof line, "    %.8ef, %.8ef, %.8ef, %.8ef,\n", p[0], p[1], p[2], p[3]);
                s += line;
            }
            s += "};\n";
            const std::string path = std::string(UNX_SOURCE_DIR) + "/Native/Render/Passes/Shading/LtcTable.inl";
            writeTextFile(path, s);
            logf("wrote %s\n", path.c_str());
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
