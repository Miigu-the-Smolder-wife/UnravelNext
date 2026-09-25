// Material v2 approximation errors (Docs/Design/MATERIAL_LAYERS_KO.md section 3), measured against physical references.
//   unx_study_material_layers thinfilm     <out.md>   thin-film methods (a) revised, (b), (c) N = 8/16/32 vs spectral Airy
//   unx_study_material_layers metals       <out.md>   fitted RGB n, k presets (gold, copper, silver, aluminium, iron)
//   unx_study_material_layers clearcoat_r1 <out.md> [photons]   clearcoat R1 and the original 1.1 vs the layer model
//   unx_study_material_layers clearcoat_r1_ms <out.md> [photons] the same against a multiple-scattering coat
//   unx_study_material_layers clearcoat_r1a <out.md> [photons]   R1 and candidate A (coat MS term) vs the MS coat
//   unx_study_material_layers clearcoat_r1b <out.md> [photons]   R1+A and candidate B (metal base path) vs the MS coat
//   unx_study_material_layers clearcoat_r1c <out.md> [photons]   R1+A2 and A2+B2 (scaled coat term, Sinkhorn base path)
//   unx_study_material_layers clearcoat_r1d <out.md> [photons]   R1+A2 and A2+B3 (single-hit lobe + separable multi-hit term)
//   unx_study_material_layers clearcoat_r1e <out.md> [photons]   A2 + S and A2 + B2 + S (angle-dependent refraction spread)
//   unx_study_material_layers clearcoat_diag <out.md> [photons]  energy split by base interactions (failure diagnosis)
//   unx_study_material_layers clearcoat_specpath <out.md> [photons] lossless GGX base under the coat: energy per base-hit count
//   unx_study_material_layers v1albedo <out.md>                  v1 metal white furnace (table E vs the model's own integral)
//   unx_study_material_layers coatfilm     <out.md> [photons]   R1 + film under the coat vs the layer model
//   unx_study_material_layers tables       <out.inc>  E_c, K, A_x, B_x, Abar, Bbar tables for the definition
// CPU only, at most 4 worker threads, below-normal priority (the machine is shared with measurements and the user).
// Optics: ThinFilm.h; layer model and definitions: Layers.cpp.
#include "Studies.h"
#include "ThinFilm.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include <windows.h>

namespace unx::study
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Stat
{
    double sumDe = 0, maxDe = 0, maxTir = 0, n = 0, worstDeg = 0, worstD = 0;
    std::vector<float> des;
    void add(double de, bool tir, int deg = 0, double d = 0)
    {
        if (tir)
        {
            maxTir = std::max(maxTir, de);
            return;
        }
        sumDe += de;
        if (de > maxDe)
        {
            worstDeg = deg;
            worstD = d;
        }
        maxDe = std::max(maxDe, de);
        n += 1;
        des.push_back((float)de);
    }
    double p99() const
    {
        std::vector<float> v = des;
        if (v.empty()) return 0;
        const size_t k = (size_t)(0.99 * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
        return v[k];
    }
    std::string cell() const
    {
        return format("%.2f / %.1f / %.1f", n ? sumDe / n : 0.0, p99(), maxDe) + (maxTir > 0 ? format(" (TIR %.1f)", maxTir) : std::string());
    }
    std::string where() const { return format(" @%.0f°, %.0f nm", worstDeg, worstD);
    }
};

std::vector<Substrate> substrates(bool fitted)
{
    std::vector<Substrate> subs = { constantSubstrate("dielectric 1.5", 1.5, 0), constantSubstrate("design default (1.5, 2)", 1.5, 2) };
    Substrate au = metal("gold", "Au_johnson_christy_um_n_k.txt"), cu = metal("copper", "Cu_johnson_christy_um_n_k.txt");
    if (fitted)
    {
        fitRgb(au);
        fitRgb(cu);
        au.name += " (fitted n,k)";
        cu.name += " (fitted n,k)";
    }
    else
    {
        au.name += " (650/550/450 samples)";
        cu.name += " (650/550/450 samples)";
    }
    subs.push_back(au);
    subs.push_back(cu);
    return subs;
}
} // namespace

void thinFilmStudy(const std::string& out)
{
    std::vector<Substrate> subs = substrates(true);
    {
        std::vector<Substrate> s2 = substrates(false);
        subs.push_back(s2[2]);
        subs.push_back(s2[3]);
    }
    const double films[3] = { 1.33, 1.5, 2.4 }, outers[2] = { 1.0, 1.5 };
    struct Case
    {
        double n0, nf;
        size_t sub;
        Stat a, b, c8, c16, c32;
    };
    std::vector<Case> cases;
    for (double n0 : outers)
        for (double nf : films)
            for (size_t si = 0; si < subs.size(); ++si) cases.push_back({ n0, nf, si, {}, {}, {}, {}, {} });
    parallelFor((uint32_t)cases.size(), [&](uint32_t ci) {
        Case& c = cases[ci];
        const Substrate& s = subs[c.sub];
        for (int deg = 0; deg <= 89; ++deg)
        {
            const double cos0 = std::cos(deg * kPi / 180);
            const bool tir = c.n0 * std::sin(deg * kPi / 180) > c.nf;
            for (int di = 0; di <= 400; ++di)
            {
                const double d = di * 5.0;
                const Rgb3 ref = filmReference(c.n0, cos0, c.nf, d, s);
                // Methods (a) and (c) are evaluated inside the TIR region too (complex q); (b) is not (its TIR error is
                // reported separately).
                c.a.add(deltaE76(filmA(c.n0, cos0, c.nf, d, s), ref), false, deg, d);
                c.b.add(deltaE76(filmB(c.n0, cos0, c.nf, d, s), ref), tir);
                c.c8.add(deltaE76(filmC(c.n0, cos0, c.nf, d, s, 8), ref), false);
                c.c16.add(deltaE76(filmC(c.n0, cos0, c.nf, d, s, 16), ref), false);
                c.c32.add(deltaE76(filmC(c.n0, cos0, c.nf, d, s, 32), ref), false);
            }
        }
    });
    std::ostringstream md;
    md << "# Thin-film RGB methods vs spectral Airy reflectance [measured]\n\n"
          "`unx_study_material_layers thinfilm`. Grid: incidence 0-89 deg (1 deg) x thickness 0-2000 nm (5 nm). Reference: exact "
          "polarised Airy reflectance per wavelength (360-830 nm, 1 nm), CIE 1931 2 deg, illuminant E, linear Rec.709 white-balanced "
          "to E. Cells: dE76 mean / P99 / max over the grid. (a) revised: Gaussian-sensitivity series m <= 3 + closed tail m >= 4, "
          "exact complex Fresnel (n + ik), evanescent films (TIR) by complex q. (b) previous engine 3 bands (TIR region excluded, "
          "its max in parentheses). (c) N wavenumber bins, Poisson-kernel bin average x bin CMF integral. Metals: Johnson & Christy "
          "spectral n,k (reference); RGB methods get the fitted triplet or the 650/550/450 nm point samples.\n\n"
          "| outer | film n | substrate | (a) revised | (b) 3 bands | (c) 8 bins | (c) 16 bins | (c) 32 bins |\n|---|---|---|---|---|---|---|---|\n";
    for (const Case& c : cases)
        md << format("| %.1f | %.2f | %s | %s | %s | %s | %s | %s |\n", c.n0, c.nf, subs[c.sub].name.c_str(), (c.a.cell() + c.a.where()).c_str(), c.b.cell().c_str(), c.c8.cell().c_str(),
                     c.c16.cell().c_str(), c.c32.cell().c_str());
    writeTextFile(out, md.str());
    logf("%s", md.str().c_str());
}

void metalPresets(const std::string& out)
{
    struct M
    {
        const char* name;
        const char* file;
    };
    const M metals[] = { { "gold", "Au_johnson_christy_um_n_k.txt" }, { "copper", "Cu_johnson_christy_um_n_k.txt" }, { "silver", "Ag_johnson_christy_um_n_k.txt" },
                         { "aluminium", "Al_rakic_um_n_k.txt" }, { "iron", "Fe_johnson_christy_um_n_k.txt" } };
    std::ostringstream md;
    md << "# Metal presets: fitted RGB n, k [measured]\n\n"
          "`unx_study_material_layers metals`. Per channel, (n, k) minimising the squared reflectance error against the spectral "
          "reference (CIE 1931, E, Rec.709) over incidence 0-89 deg in air; degenerate channels take the smallest k within 0.1 % of the "
          "best error. dE76 of the bare metal (d = 0) over incidence 0-89 deg in air and under a 1.5 coat: mean / max.\n\n"
          "| metal | n (R, G, B) | k (R, G, B) | fit max abs dR (R, G, B) | dE air mean / max | dE coat mean / max | point samples 650/550/450: dE air mean / max |\n"
          "|---|---|---|---|---|---|---|\n";
    std::ostringstream inc;
    inc << "// Metal presets (substrateIor, substrateExtinction), fitted per channel; unx_study_material_layers metals.\n";
    for (const M& m : metals)
    {
        Substrate fit = metal(m.name, m.file), pts = metal(m.name, m.file);
        const Rgb3 worst = fitRgb(fit, 1.0);
        auto stats = [&](const Substrate& s, double n0, double& mean, double& mx) {
            mean = mx = 0;
            for (int deg = 0; deg < 90; ++deg)
            {
                const double c = std::cos(deg * kPi / 180);
                const double de = deltaE76(filmA(n0, c, 1.0, 0, s), filmReference(n0, c, 1.0, 0, s));
                mean += de / 90;
                mx = std::max(mx, de);
            }
        };
        double am, ax, cm, cx, pm, px;
        stats(fit, 1.0, am, ax);
        stats(fit, 1.5, cm, cx);
        stats(pts, 1.0, pm, px);
        md << format("| %s | %.3f, %.3f, %.3f | %.3f, %.3f, %.3f | %.4f, %.4f, %.4f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f |\n", m.name, fit.rgbN[0], fit.rgbN[1], fit.rgbN[2],
                     fit.rgbK[0], fit.rgbK[1], fit.rgbK[2], worst[0], worst[1], worst[2], am, ax, cm, cx, pm, px);
        inc << format("// %s\n{ { %.4ff, %.4ff, %.4ff }, { %.4ff, %.4ff, %.4ff } },\n", m.name, fit.rgbN[0], fit.rgbN[1], fit.rgbN[2], fit.rgbK[0], fit.rgbK[1], fit.rgbK[2]);
    }
    writeTextFile(out, md.str() + "\n```cpp\n" + inc.str() + "```\n");
    logf("%s", md.str().c_str());
}
} // namespace unx::study

int main(int argc, char** argv)
{
    using namespace unx;
    try
    {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        if (argc < 3) fail("usage: unx_study_material_layers thinfilm|metals|clearcoat_r1|clearcoat_r1_ms|clearcoat_r1a|clearcoat_r1b|clearcoat_r1c|clearcoat_r1d|clearcoat_r1e|clearcoat_diag|clearcoat_specpath|v1albedo|coatfilm|tables <out> [photons]");
        const std::string cmd = argv[1], out = argv[2];
        const uint32_t photons = argc > 3 ? (uint32_t)std::stoul(argv[3]) : (1u << 21);
        if (cmd == "thinfilm") study::thinFilmStudy(out);
        else if (cmd == "metals") study::metalPresets(out);
        else if (cmd == "clearcoat_r1") study::clearcoatR1Study(out, photons, false);
        else if (cmd == "clearcoat_r1_ms") study::clearcoatR1Study(out, photons, true);
        else if (cmd == "clearcoat_r1a") study::clearcoatR1Study(out, photons, true, true);
        else if (cmd == "clearcoat_r1b") study::clearcoatR1Study(out, photons, true, true, true);
        else if (cmd == "clearcoat_r1c") study::clearcoatR1Study(out, photons, true, true, false, true);
        else if (cmd == "clearcoat_r1d") study::clearcoatR1Study(out, photons, true, true, false, false, true);
        else if (cmd == "clearcoat_r1e") study::clearcoatR1Study(out, photons, true, true, false, false, false, true);
        else if (cmd == "clearcoat_diag") study::clearcoatDiag(out, photons);
        else if (cmd == "clearcoat_specpath") study::clearcoatSpecPath(out, photons);
        else if (cmd == "v1albedo") study::v1Albedo(out);
        else if (cmd == "coatfilm") study::coatFilmStudy(out, photons);
        else if (cmd == "tables") study::exportTables(out);
        else fail("unknown study %s", cmd.c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
