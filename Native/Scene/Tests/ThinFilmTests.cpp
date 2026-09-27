// A9 thin film (render A; MATERIAL_LAYERS 1.2, MaterialModel.h Film): the model's method (c) against the exact spectral
// Airy reflectance (1 nm, CIE 1931 2 deg, illuminant E, linear Rec.709 white-balanced to E: the C study's reference,
// Reference/Studies/ThinFilm.cpp), the renderer's mu table against the model, energy, the scene block and validation.
//   unx_test_scene_film [--image <path.ppm>]   (--image: a soap bubble swatch, thickness x angle, for the record)
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::scene;
using cd = std::complex<double>;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
constexpr double kPi = 3.14159265358979323846;

struct Cmf
{
    std::vector<double> lambda, x, y, z;
    double sumY = 0, m[3][3] = {}, inv[3][3] = {};
};
Cmf g_cmf;

void loadCmf()
{
    std::ifstream f(std::string(UNX_SOURCE_DIR) + "/Reference/Studies/data/cie1931_2deg_xyz_1nm.csv");
    if (!f) fail("missing CIE table");
    std::string line;
    while (std::getline(f, line))
    {
        double l, a, b, d;
        char c1, c2, c3;
        std::istringstream s(line);
        if (s >> l >> c1 >> a >> c2 >> b >> c3 >> d) g_cmf.lambda.push_back(l), g_cmf.x.push_back(a), g_cmf.y.push_back(b), g_cmf.z.push_back(d);
    }
    double w[3] = { 0, 0, 0 };
    for (size_t i = 0; i < g_cmf.lambda.size(); ++i) w[0] += g_cmf.x[i], w[1] += g_cmf.y[i], w[2] += g_cmf.z[i];
    g_cmf.sumY = w[1];
    for (double& v : w) v /= g_cmf.sumY;
    const double s[3][3] = { { 3.2404542, -1.5371385, -0.4985314 }, { -0.9692660, 1.8760108, 0.0415560 }, { 0.0556434, -0.2040259, 1.0572252 } };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) g_cmf.m[r][c] = s[r][c] / (s[r][0] * w[0] + s[r][1] * w[1] + s[r][2] * w[2]);
    const auto& m = g_cmf.m;
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            g_cmf.inv[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / det;
        }
}

struct Metal
{
    std::vector<double> wl, n, k;
    cd at(double nm) const
    {
        const double l = nm * 1e-3;
        size_t i = 1;
        while (i + 1 < wl.size() && wl[i] < l) ++i;
        const double t = std::clamp((l - wl[i - 1]) / (wl[i] - wl[i - 1]), 0.0, 1.0);
        return cd(n[i - 1] + t * (n[i] - n[i - 1]), k[i - 1] + t * (k[i] - k[i - 1]));
    }
};
Metal loadMetal(const char* file)
{
    Metal m;
    std::ifstream f(std::string(UNX_SOURCE_DIR) + "/Reference/Studies/data/" + file);
    double a, b, c;
    while (f >> a >> b >> c) m.wl.push_back(a), m.n.push_back(b), m.k.push_back(c);
    if (m.wl.size() < 10) fail("bad metal table %s", file);
    return m;
}

cd sqrtUpper(cd z)
{
    const cd q = std::sqrt(z);
    return q.imag() < 0 ? -q : q;
}

double airy(double n0, double cos0, double nf, cd n2, double d, double lambda)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    const cd q0 = n0 * cos0, q1 = sqrtUpper(cd(nf * nf - s2, 0)), q2 = sqrtUpper(n2 * n2 - s2);
    const cd e0 = n0 * n0, e1 = nf * nf, e2 = n2 * n2;
    const cd r01[2] = { (q0 - q1) / (q0 + q1), (e1 * q0 - e0 * q1) / (e1 * q0 + e0 * q1) };
    const cd r12[2] = { (q1 - q2) / (q1 + q2), (e2 * q1 - e1 * q2) / (e2 * q1 + e1 * q2) };
    const cd ph = std::exp(cd(0, 1) * (4 * kPi * d / lambda) * q1);
    double R = 0;
    for (int p = 0; p < 2; ++p) R += 0.5 * std::norm((r01[p] + r12[p] * ph) / (1.0 + r01[p] * r12[p] * ph));
    return R;
}

// Exact spectral reference (1 nm), clamped to [0, 1] like the model (out-of-gamut spectra).
float3 reference(double n0, double cos0, const model::Film& f, const Metal* metal)
{
    double xyz[3] = { 0, 0, 0 };
    for (size_t i = 0; i < g_cmf.lambda.size(); ++i)
    {
        const cd sub = metal ? metal->at(g_cmf.lambda[i]) : cd(f.substrateIor, f.substrateExtinction);
        const double R = airy(n0, cos0, f.ior, sub, f.thickness, g_cmf.lambda[i]);
        xyz[0] += R * g_cmf.x[i], xyz[1] += R * g_cmf.y[i], xyz[2] += R * g_cmf.z[i];
    }
    float out[3];
    for (int r = 0; r < 3; ++r)
        out[r] = (float)std::clamp((g_cmf.m[r][0] * xyz[0] + g_cmf.m[r][1] * xyz[1] + g_cmf.m[r][2] * xyz[2]) / g_cmf.sumY, 0.0, 1.0);
    return float3{ out[0], out[1], out[2] };
}

double deltaE76(float3 a, float3 b)
{
    auto lab = [](float3 c) {
        const double rgb[3] = { c.x, c.y, c.z };
        double xyz[3], w[3];
        for (int r = 0; r < 3; ++r)
        {
            xyz[r] = g_cmf.inv[r][0] * rgb[0] + g_cmf.inv[r][1] * rgb[1] + g_cmf.inv[r][2] * rgb[2];
            w[r] = g_cmf.inv[r][0] + g_cmf.inv[r][1] + g_cmf.inv[r][2];
        }
        auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16) / 116.0; };
        const double fx = f(xyz[0] / w[0]), fy = f(xyz[1] / w[1]), fz = f(xyz[2] / w[2]);
        return std::array<double, 3>{ 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz) };
    };
    const auto la = lab(a), lb = lab(b);
    return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
}

struct Case
{
    const char* name;
    double outer;
    float ior;
    model::FilmSubstrate substrate;
    float n, k;
    const char* metalFile;
};

float3 tableAt(const std::vector<float>& t, float mu)
{
    const float x = std::clamp(mu, 0.0f, 1.0f) * (model::kFilmTableMu - 1);
    const uint32_t i = std::min((uint32_t)x, model::kFilmTableMu - 2);
    const float a = x - i;
    return float3{ t[3 * i] * (1 - a) + t[3 * i + 3] * a, t[3 * i + 1] * (1 - a) + t[3 * i + 4] * a, t[3 * i + 2] * (1 - a) + t[3 * i + 5] * a };
}

void writeImage(const char* path)
{
    // soap film (air / 1.33 / air): thickness 0..1500 nm across, incidence 0..89 deg down; linear -> sRGB, x 3 exposure
    const int W = 600, H = 240;
    std::vector<unsigned char> px(3 * W * H);
    model::Film f;
    f.ior = 1.33f, f.substrateIor = 1, f.substrateExtinction = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            f.thickness = 1500.0f * x / (W - 1);
            const float3 c = model::filmReflectance(f, 1, (float)std::cos(89.0 * kPi / 180 * y / (H - 1)));
            const float v[3] = { c.x, c.y, c.z };
            for (int k = 0; k < 3; ++k)
            {
                const double l = std::clamp(3.0 * v[k], 0.0, 1.0);
                px[3 * (y * W + x) + k] = (unsigned char)std::lround(255 * (l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055));
            }
        }
    std::ofstream o(path, std::ios::binary);
    o << "P6\n" << W << " " << H << "\n255\n";
    o.write((const char*)px.data(), px.size());
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        loadCmf();
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--image") writeImage(argv[i + 1]);
        const Case cases[] = {
            { "soap bubble air/1.33/air", 1, 1.33f, model::FilmSubstrate::Constant, 1, 0, nullptr },
            { "oil on water air/1.5/1.33", 1, 1.5f, model::FilmSubstrate::Constant, 1.33f, 0, nullptr },
            { "water film air/1.33/1.5", 1, 1.33f, model::FilmSubstrate::Constant, 1.5f, 0, nullptr },
            { "TiO2 coat air/2.4/1.5", 1, 2.4f, model::FilmSubstrate::Constant, 1.5f, 0, nullptr },
            { "design default air/1.5/(1.5, 2)", 1, 1.5f, model::FilmSubstrate::Constant, 1.5f, 2, nullptr },
            { "oxide on gold air/1.33", 1, 1.33f, model::FilmSubstrate::Gold, 0, 0, "Au_johnson_christy_um_n_k.txt" },
            { "oxide on copper air/1.5", 1, 1.5f, model::FilmSubstrate::Copper, 0, 0, "Cu_johnson_christy_um_n_k.txt" },
            { "anodised aluminium air/1.65", 1, 1.65f, model::FilmSubstrate::Aluminium, 0, 0, "Al_rakic_um_n_k.txt" },
            { "temper colours on iron air/2.4", 1, 2.4f, model::FilmSubstrate::Iron, 0, 0, "Fe_johnson_christy_um_n_k.txt" },
            { "evanescent 1.5/1.33/1.5", 1.5, 1.33f, model::FilmSubstrate::Constant, 1.5f, 0, nullptr },
        };
        double worstMean = 0, worstP99 = 0, worstMax = 0, worstTable = 0;
        for (const Case& c : cases)
        {
            Metal metal;
            if (c.metalFile) metal = loadMetal(c.metalFile);
            model::Film f;
            f.ior = c.ior, f.substrate = c.substrate, f.substrateIor = c.n, f.substrateExtinction = c.k;
            std::vector<double> de;
            double tableMax = 0;
            for (int d = 0; d <= 2000; d += 10)
            {
                f.thickness = (float)d;
                for (int deg = 0; deg <= 89; deg += 2)
                {
                    const double cos0 = std::cos(deg * kPi / 180);
                    de.push_back(deltaE76(model::filmReflectance(f, (float)c.outer, (float)cos0), reference(c.outer, cos0, f, c.metalFile ? &metal : nullptr)));
                }
                // The renderer's films see air (validation: no film under a coat yet), so no critical angle falls inside the
                // table; under a coat (outer 1.5 over a film < 1.5) the reflectance jumps at the critical angle and a uniform
                // mu table is not usable (76 dE measured) - that join places the table's points around the critical angle.
                if (c.outer != 1) continue;
                const std::vector<float> t = model::filmTable(f, (float)c.outer);
                for (int k = 0; k < 400; ++k)  // between the table's points
                {
                    const float mu = (k + 0.5f) / 400;
                    tableMax = std::max(tableMax, deltaE76(tableAt(t, mu), model::filmReflectance(f, (float)c.outer, mu)));
                }
            }
            std::sort(de.begin(), de.end());
            double mean = 0;
            for (double v : de) mean += v;
            mean /= de.size();
            const double p99 = de[(size_t)(0.99 * (de.size() - 1))], mx = de.back();
            std::printf("%-34s (c) vs spectral dE76 mean %.2f P99 %.2f max %.2f | table (%u mu) vs (c) max %.2f\n", c.name, mean, p99, mx,
                        model::kFilmTableMu, tableMax);
            worstMean = std::max(worstMean, mean), worstP99 = std::max(worstP99, p99), worstMax = std::max(worstMax, mx);
            worstTable = std::max(worstTable, tableMax);
        }
        std::printf("worst: mean %.2f P99 %.2f max %.2f, table %.2f\n", worstMean, worstP99, worstMax, worstTable);
        // C's pass line for (c) 32 bins (REPORT_R2 3: mean <= 0.54, P99 <= 1.5, max <= 2.3), over our cases
        CHECK(worstMean <= 0.6 && worstP99 <= 1.6 && worstMax <= 2.5);
        CHECK(worstTable <= 1.0);  // the renderer's table: below the JND (2.3) and the method's own error

        // Energy: F in [0, 1]; the specular albedo with F' (w = 1) stays <= 1 (quasi-random hemisphere, mirror to rough)
        {
            model::Film f;
            f.thickness = 380, f.ior = 1.33f, f.substrate = model::FilmSubstrate::Silver;
            model::Surface s;
            s.baseColor = { 0, 0, 0 }, s.metallic = 0;
            double worst = 0;
            for (float r : { 0.2f, 0.5f, 1.0f })
                for (float mu : { 0.1f, 0.5f, 1.0f })
                {
                    s.roughness = r;
                    const float3 n{ 0, 0, 1 }, v{ std::sqrt(1 - mu * mu), 0, mu };
                    double sum[3] = { 0, 0, 0 };
                    const int N = 256;
                    for (int i = 0; i < N; ++i)
                        for (int j = 0; j < N; ++j)
                        {
                            const double z = (i + 0.5) / N, phi = 2 * kPi * (j + 0.5) / N, sz = std::sqrt(1 - z * z);  // uniform in cos
                            const float3 l{ (float)(sz * std::cos(phi)), (float)(sz * std::sin(phi)), (float)z };
                            const float3 e = model::evaluateFilm(s, f, n, v, l);
                            sum[0] += e.x * z, sum[1] += e.y * z, sum[2] += e.z * z;
                        }
                    for (double& x : sum) worst = std::max(worst, x * 2 * kPi / (N * N));
                }
            std::printf("specular albedo with the film (silver, 380 nm): max %.4f\n", worst);
            CHECK(worst <= 1.01);  // quadrature error of the mirror-like r 0.2 lobe included
        }

        // Scene block round trip and validation
        {
            Scene s;
            Material m;
            m.name = "bubble";
            m.thinFilmThickness = 420, m.thinFilmIor = 1.33f, m.thinFilmCoverage = 0.9f, m.thinFilmSubstrate = 3, m.substrateIor = 1.2f, m.substrateExtinction = 0.5f;
            s.materials.push_back(m);
            const Scene r = deserialize(serialize(s));
            const Material& q = r.materials.at(0);
            CHECK(q.thinFilmThickness == 420 && q.thinFilmIor == 1.33f && q.thinFilmCoverage == 0.9f && q.thinFilmSubstrate == 3 && q.substrateIor == 1.2f &&
                  q.substrateExtinction == 0.5f);
            auto refused = [](Material bad) {
                Scene t;
                t.materials.push_back(bad);
                try
                {
                    validate(t);
                }
                catch (const std::exception&)
                {
                    return true;
                }
                return false;
            };
            Material bad = m;
            bad.clearcoat = 0.5f;
            CHECK(refused(bad));
            bad = m, bad.thinFilmSubstrate = 6;
            CHECK(refused(bad));
            bad = m, bad.thinFilmThickness = -1;
            CHECK(refused(bad));
            CHECK(!refused(m));
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
    std::printf(g_failures ? "FAILED (%u)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
