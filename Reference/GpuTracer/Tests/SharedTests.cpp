// unx_test_reference_gpu: the shared estimator code (Reference/GpuTracer/shared, compiled here as C++) against the CPU
// estimator's own functions (Reference/PathTracer/src), function by function. The GPU compiles the same shared source;
// its agreement with the CPU images is checked by the validation gate (README_KO.md). No GPU is used here.
//   sampler      bitwise equal (integer arithmetic)
//   material     BRDF value, lobe probabilities, pdf, sampled directions against the double-precision CPU evaluation
//   atmosphere   coefficients, ground/top distances, tau to the top, segment optical depth, phase sampling
//   lights       importance, candidate total and choice, sampling and intersection (city_night: 512 lights)
#include "Atmosphere.h"
#include "Bsdf.h"
#include "Lights.h"
#include "Sampler.h"

#include "shared/Types.hlsli"
#include "shared/Sampler.hlsli"

#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"
#include "unx/scenegen/SceneGen.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace unx;
namespace sh = unx::reference::shared;

// Data functions the shared headers read.
namespace unx::reference::shared
{
static const std::vector<float>* g_albedo = nullptr;
static const std::vector<std::array<float, 3>>* g_atm = nullptr;
static std::vector<RtLight> g_lights;
static std::vector<uint32_t> g_cellStart, g_cellLights;
float rtAlbedoTableFetch(uint i) { return (*g_albedo)[i]; }
float3 rtAtmTableFetch(uint ir, uint im)
{
    const auto& t = (*g_atm)[(size_t)ir * kRtAtmTableMu + im];
    return { t[0], t[1], t[2] };
}
RtLight rtLightFetch(uint i) { return g_lights[i]; }
uint rtLightCellStart(uint c) { return g_cellStart[c]; }
uint rtLightCellLight(uint k) { return g_cellLights[k]; }
} // namespace unx::reference::shared

namespace
{
int g_failures = 0;
#define CHECK(c)                                                                                                                       \
    do                                                                                                                                 \
    {                                                                                                                                  \
        if (!(c))                                                                                                                      \
        {                                                                                                                              \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                  \
            ++g_failures;                                                                                                              \
        }                                                                                                                              \
    } while (0)

sh::float3 S(float3 v) { return { v.x, v.y, v.z }; }
float3 U(sh::float3 v) { return { v.x, v.y, v.z }; }
double relErr(double a, double b) { return std::fabs(a - b) / std::max(std::fabs(b), 1e-30); }
double maxRel3(sh::float3 a, reference::Rgb b, double floor)
{
    const double e[3] = { std::fabs(a.x - b.r) / std::max((double)std::fabs(b.r), floor), std::fabs(a.y - b.g) / std::max((double)std::fabs(b.g), floor),
                          std::fabs(a.z - b.b) / std::max((double)std::fabs(b.b), floor) };
    return std::max({ e[0], e[1], e[2] });
}

struct Rng
{
    reference::Pcg32 g{ 12345, 7 };
    float u() { return g.uniform(); }
    float3 dir()
    {
        const float z = 2 * u() - 1, r = std::sqrt(std::max(0.0f, 1 - z * z)), p = 6.2831853f * u();
        return { r * std::cos(p), r * std::sin(p), z };
    }
};

void testSampler()
{
    uint64_t compared = 0;
    for (uint32_t seed : { 0u, 1u, 0x5EEDu, 0xDEADBEEFu })
        for (uint32_t idx = 0; idx < 300; idx += 7)
        {
            reference::Sampler a(seed, idx);
            sh::RtSampler b = sh::rtSamplerInit(seed, idx);
            for (int k = 0; k < 40; ++k)
            {
                if (k % 3 == 0)
                {
                    const float x = a.get1D(), y = sh::rtGet1D(b);
                    CHECK(x == y);
                }
                else
                {
                    float x0, x1, y0, y1;
                    a.get2D(x0, x1);
                    sh::rtGet2D(b, y0, y1);
                    CHECK(x0 == y0 && x1 == y1);
                }
                ++compared;
            }
        }
    reference::Pcg32 p(0xABCDEF0123ull, 99);
    sh::RtPcg32 q = sh::rtPcgInit(0xABCDEF0123ull, 99);
    for (int i = 0; i < 1000; ++i) CHECK(p.next() == sh::rtPcgNext(q));
    logf("  sampler: %llu draws bitwise equal\n", (unsigned long long)compared);
}

void testMaterial()
{
    sh::g_albedo = &scene::model::directionalAlbedoTable();
    Rng r;
    double worstEval = 0, worstPdf = 0, worstDir = 0, worstProb = 0;
    uint32_t n = 0, sampled = 0;
    for (int i = 0; i < 200000; ++i)
    {
        reference::Surface s;
        s.ns = normalize(r.dir());
        s.ng = normalize(s.ns + r.dir() * 0.2f);
        if (dot(s.ng, s.ns) <= 0.2f) continue;
        s.bsdf.cls = (r.u() < 0.3f) ? scene::MaterialClass::Foliage : scene::MaterialClass::Standard;
        s.bsdf.baseColor = { r.u(), r.u(), r.u() };
        const float rr = r.u();
        s.bsdf.roughness = rr < 0.1f ? 0.0f : rr < 0.2f ? 0.02f * r.u() : r.u();
        s.bsdf.metallic = r.u() < 0.5f ? 0.0f : r.u();
        s.bsdf.specular = r.u();
        s.bsdf.transmission = s.bsdf.cls == scene::MaterialClass::Foliage ? r.u() : 0.0f;
        float3 wo = r.dir();
        if (dot(wo, s.ns) < 0.02f || dot(wo, s.ng) <= 0) continue;
        const reference::Bsdf cpu(s, wo);
        sh::RtSurface gs;
        gs.p = S(s.p);
        gs.ng = S(s.ng);
        gs.ns = S(s.ns);
        gs.frontFacing = true;
        gs.bsdf = { (uint32_t)s.bsdf.cls, S(s.bsdf.baseColor), s.bsdf.roughness, s.bsdf.metallic, s.bsdf.specular, s.bsdf.transmission };
        gs.emission = {};
        gs.material = 0;
        const sh::RtBsdf gpu = sh::rtBsdfInit(gs, S(wo), false);
        // Sampled direction (same uniforms) and its value / pdf.
        const float ul = r.u(), u1 = r.u(), u2 = r.u();
        reference::BsdfSample cs;
        const bool okC = cpu.sample(ul, u1, u2, cs);
        sh::float3 gwi, gf;
        float gp;
        const bool okG = sh::rtBsdfSample(gpu, ul, u1, u2, gwi, gf, gp);
        if (okC && okG)
        {
            worstDir = std::max(worstDir, (double)length(cs.wi - U(gwi)));
            // Values: relative, with a floor at 1e-3 of the local max (the float GGX peak at alpha 1e-4 is 3e7).
            const double fl = std::max(1e-6, 1e-3 * std::max({ cs.f.r, cs.f.g, cs.f.b }));
            if (length(cs.wi - U(gwi)) < 1e-5f)
            {
                worstEval = std::max(worstEval, maxRel3(gf, cs.f, fl));
                worstPdf = std::max(worstPdf, relErr(gp, cs.pdf));
            }
            ++sampled;
        }
        CHECK(okC == okG || (okC ? cs.pdf : gp) < 1e-6f || std::fabs(dot(okC ? cs.wi : U(gwi), s.ns)) < 1e-4f);
        // Evaluation at a CPU direction.
        const float3 wi = r.dir();
        const reference::Rgb fc = cpu.eval(wi);
        const sh::float3 fg = sh::rtBsdfEval(gpu, S(wi));
        const double fl = std::max(1e-6, 1e-3 * std::max({ fc.r, fc.g, fc.b }));
        worstEval = std::max(worstEval, maxRel3(fg, fc, fl));
        const float pc = cpu.pdf(wi), pg = sh::rtBsdfPdf(gpu, S(wi));
        worstPdf = std::max(worstPdf, (double)(std::fabs(pc - pg) / std::max(pc, 1e-3f)));
        ++n;
    }
    (void)worstProb;
    logf("  material: %u evaluations, %u samples: worst relative BRDF %.2e, pdf %.2e, sampled direction %.2e\n", n, sampled, worstEval, worstPdf, worstDir);
    CHECK(worstEval < 2e-3);
    CHECK(worstPdf < 2e-3);
    CHECK(worstDir < 1e-4);
}

sh::RtAtmosphere shAtm(const scene::Atmosphere& p)
{
    sh::RtAtmosphere a;
    a.R = p.bottomRadius;
    a.Rt = p.topRadius;
    const double h2 = (double)p.topRadius * p.topRadius - (double)p.bottomRadius * p.bottomRadius;
    a.H2 = (float)h2;
    a.H = (float)std::sqrt(h2);
    a.rayleighScaleHeight = p.rayleighScaleHeight;
    a.mieScaleHeight = p.mieScaleHeight;
    a.mieG = p.mieG;
    a.ozoneCenter = p.ozoneCenter;
    a.ozoneWidth = p.ozoneWidth;
    a.rayleighScattering = S(p.rayleighScattering);
    a.mieScattering = S(p.mieScattering);
    a.mieAbsorption = S(p.mieAbsorption);
    a.ozoneAbsorption = S(p.ozoneAbsorption);
    a.groundAlbedo = S(p.groundAlbedo);
    return a;
}

void testAtmosphere()
{
    const scene::Atmosphere params;
    const reference::AtmosphereModel cpu(params);
    sh::g_atm = &cpu.table();
    const sh::RtAtmosphere a = shAtm(params);
    Rng r;
    double worstCoef = 0, worstGround = 0, worstTop = 0, worstTau = 0, worstSeg = 0, worstT = 0;
    uint32_t n = 0, outside = 0;
    for (int i = 0; i < 20000; ++i)
    {
        // Scene-range points: up to 25 km horizontally, -50 m to 12 km in height, and some far up.
        const float3 p{ (r.u() - 0.5f) * 50000.0f, (r.u() < 0.1f ? -50.0f * r.u() : r.u() < 0.8f ? 3000.0f * r.u() : 12000.0f * r.u()), (r.u() - 0.5f) * 50000.0f };
        float3 d = r.dir();
        if (r.u() < 0.3f) d = normalize(float3{ d.x, d.y * 0.01f, d.z });  // grazing
        const reference::Double3 pd{ p.x, p.y, p.z };
        // Coefficients at the altitude.
        const double h = cpu.altitude(pd);
        const float hg = sh::rtAtmAltitude(a, S(p));
        CHECK(std::fabs(h - hg) <= 1e-3 + 1e-6 * std::fabs(h));
        const auto cc = cpu.at(h);
        const sh::RtAtmCoefficients gc = sh::rtAtmAt(a, (float)h);
        worstCoef = std::max(worstCoef, maxRel3(gc.extinction, cc.extinction, 1e-12));
        // Ground and top distances.
        const double g0 = cpu.groundDistance(pd, d);
        const float g1 = sh::rtAtmGroundDistance(a, S(p), S(d));
        CHECK((g0 > 0) == (g1 > 0));
        if (g0 > 0 && g1 > 0) worstGround = std::max(worstGround, relErr(g1, g0));
        const double t0 = cpu.topDistance(pd, d);
        const float t1 = sh::rtAtmTopDistance(a, S(p), S(d));
        if (t0 > 0) worstTop = std::max(worstTop, relErr(t1, t0));
        // tau to the top: transmittance error where it is not negligible.
        const reference::Rgb tc = cpu.opticalDepthToTop(pd, d);
        const sh::float3 tg = sh::rtAtmDepthToTop(a, S(p), S(d));
        CHECK(tc.finite() == sh::rtFinite3(tg));
        if (tc.finite() && sh::rtFinite3(tg))
        {
            if (tc.max() < 50) worstTau = std::max(worstTau, (double)std::max({ std::fabs(tc.r - tg.x), std::fabs(tc.g - tg.y), std::fabs(tc.b - tg.z) }));
            for (int c = 0; c < 3; ++c)
            {
                const double a0 = (&tc.r)[c], a1 = c == 0 ? tg.x : c == 1 ? tg.y : tg.z;
                if (std::exp(-a0) > 1e-4) worstT = std::max(worstT, std::fabs(std::expm1(-(a1 - a0))));
            }
        }
        // Segment optical depth (short and long segments, not through the planet).
        const double len = r.u() < 0.5f ? 50.0 * r.u() + 1 : 60000.0 * r.u() + 1;
        if (g0 > 0 && len >= g0) continue;
        if (t0 > 0 && len > t0) continue;
        // Long segments (>= 20 km, tau_top differences) that start or end below the planet surface are outside the
        // estimator's domain: both implementations subtract two tau_top values of 1e20 and more (a chord through the
        // ground), so neither result means anything; the path loop never forms one (a downward ray from a valley floor
        // hits the terrain first; without a hit below the surface the path ends).
        if (len >= reference::AtmosphereModel::kShortSegment && (h < 0 || cpu.altitude({ pd.x + d.x * len, pd.y + d.y * len, pd.z + d.z * len }) < 0))
        {
            ++outside;
            continue;
        }
        const reference::Rgb sc = cpu.opticalDepth(pd, d, len);
        const sh::float3 sg = sh::rtAtmOpticalDepth(a, S(p), S(d), (float)len);
        const double segErr = (double)std::max({ std::fabs(sc.r - sg.x), std::fabs(sc.g - sg.y), std::fabs(sc.b - sg.z) }) / std::max(1e-3, (double)sc.max());
        static int printed = 0;
        if (segErr > 1e-4 && printed++ < 6)
            logf("    segment p (%.3f %.3f %.3f) d (%.6f %.6f %.6f) len %.1f ground %.1f top %.1f: cpu %.6g %.6g %.6g gpu %.6g %.6g %.6g\n", p.x, p.y, p.z, d.x, d.y, d.z, len, g0, t0,
                 sc.r, sc.g, sc.b, sg.x, sg.y, sg.z);
        worstSeg = std::max(worstSeg, segErr);
        ++n;
    }
    logf("  atmosphere: %u segments: altitude ok, extinction %.2e rel, ground distance %.2e rel, top distance %.2e rel, tau_top %.2e abs where < 50 (transmittance %.2e rel), segment tau %.2e rel (%u long segments below the surface skipped)\n",
         n, worstCoef, worstGround, worstTop, worstTau, worstT, worstSeg, outside);
    CHECK(worstCoef < 1e-5);
    CHECK(worstGround < 1e-5);
    CHECK(worstTop < 1e-5);
    CHECK(worstT < 1e-4);
    CHECK(worstSeg < 1e-4);
    // Phase sampling: same direction and pdf from the same uniforms.
    double worstPhaseDir = 0, worstPhasePdf = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const float3 f = r.dir();
        const float wR = r.u(), wM = r.u(), u1 = r.u(), u2 = r.u(), u3 = r.u();
        float pc, pg;
        const float3 dc = cpu.samplePhase(f, wR, wM, u1, u2, u3, pc);
        const sh::float3 dg = sh::rtSamplePhase(a, S(f), wR, wM, u1, u2, u3, pg);
        worstPhaseDir = std::max(worstPhaseDir, (double)length(dc - U(dg)));
        worstPhasePdf = std::max(worstPhasePdf, relErr(pg, pc));
    }
    logf("  phase sampling: worst direction %.2e, pdf %.2e rel\n", worstPhaseDir, worstPhasePdf);
    CHECK(worstPhaseDir < 1e-3);
    CHECK(worstPhasePdf < 1e-3);
    // Segment pdf: normalised, and sampling follows it (histogram).
    const sh::float3 o{ 0, 2, 0 };
    for (const sh::float3 dd : { sh::float3(0, 1, 0), normalize(sh::float3(1, 0.05f, 0)), normalize(sh::float3(1, 0.001f, 0.3f)) })
    {
        const float len = sh::rtAtmTopDistance(a, o, dd);
        const sh::RtSegmentPdf sp = sh::rtSegmentPdf(a, o, dd, len);
        double integral = 0;
        const int N = 200000;
        for (int k = 0; k < N; ++k)
        {
            const double t = (k + 0.5) / N * len;
            integral += sh::rtSegmentPdfAt(sp, (float)t) * len / N;
        }
        std::vector<double> hist(20, 0);
        for (int k = 0; k < 200000; ++k)
        {
            float pdf;
            const float t = sh::rtSegmentSample(sp, r.u(), pdf);
            hist[std::min(19, (int)(t / len * 20))] += 1.0 / 200000;
        }
        double worstBin = 0;
        for (int b = 0; b < 20; ++b)
        {
            double expect = 0;
            for (int k = 0; k < 1000; ++k) expect += sh::rtSegmentPdfAt(sp, (float)((b + (k + 0.5) / 1000.0) / 20 * len)) * len / 20 / 1000;
            // In units of the binomial standard deviation of the bin (200k samples).
            worstBin = std::max(worstBin, std::fabs(hist[b] - expect) / std::sqrt(std::max(expect * (1 - expect), 1e-9) / 200000));
        }
        logf("  segment pdf (len %.0f m): integral %.6f, worst histogram bin %.2f sigma\n", len, integral, worstBin);
        CHECK(std::fabs(integral - 1) < 2e-3);
        CHECK(worstBin < 5);
    }
}

void testLights()
{
    const scene::Scene s = scenegen::generate({ scenegen::SceneId::CityNight, 1, 1.0f });
    const reference::LightSet cpu(s);
    sh::g_lights.assign(cpu.count(), {});
    for (uint32_t i = 0; i < (uint32_t)cpu.count(); ++i)
    {
        const scene::Light& l = cpu.light(i);
        sh::RtLight& g = sh::g_lights[i];
        g.position = S(l.position);
        g.type = (uint32_t)l.type;
        g.forward = S(l.forward);
        g.intensity = l.intensity;
        g.right = S(l.right);
        g.range = l.range;
        g.up = S(cpu.up(i));
        g.spotScale = cpu.spotScale(i);
        g.color = S(l.color);
        g.spotOffset = cpu.spotOffset(i);
        g.size = { l.size.x, l.size.y };
        g.castShadow = l.castShadow ? 1 : 0;
    }
    sh::g_cellStart = cpu.cellStart();
    sh::g_cellLights = cpu.cellLights();
    sh::RtLightGrid grid{};
    grid.minCorner = S(cpu.gridMin());
    grid.cell = S(cpu.gridCell());
    grid.count = (uint32_t)cpu.count();
    grid.dimX = cpu.gridDim()[0];
    grid.dimY = cpu.gridDim()[1];
    grid.dimZ = cpu.gridDim()[2];
    Rng r;
    uint32_t points = 0, sameChoice = 0, samples = 0;
    double worstTotal = 0, worstProb = 0, worstL = 0, worstPdf = 0, worstDir = 0;
    reference::LightCandidates cands;
    for (int i = 0; i < 20000; ++i)
    {
        const float3 x{ (r.u() - 0.5f) * 400.0f, r.u() * 12.0f, (r.u() - 0.5f) * 400.0f };
        cpu.gather(x, cands);
        const uint32_t cell = sh::rtLightCell(grid, S(x));
        const float total = sh::rtLightTotal(cell, S(x));
        CHECK(cands.lights.empty() == !(total > 0));
        if (cands.lights.empty()) continue;
        worstTotal = std::max(worstTotal, relErr(total, cands.total));
        const float u = r.u();
        float pc, pg;
        const uint32_t kc = reference::LightSet::choose(cands, u, pc);
        const uint32_t lg = sh::rtLightChoose(cell, S(x), total, u, pg);
        sameChoice += cands.lights[kc] == lg;
        if (cands.lights[kc] == lg) worstProb = std::max(worstProb, relErr(pg, pc));
        ++points;
        // Sample the CPU's choice with both.
        const uint32_t li = cands.lights[kc];
        const float u1 = r.u(), u2 = r.u();
        reference::LightSample a;
        sh::RtLightSample b;
        const bool oa = cpu.sample(li, x, u1, u2, a), ob = sh::rtLightSample(sh::g_lights[li], S(x), u1, u2, b);
        CHECK(oa == ob);
        if (oa && ob)
        {
            worstDir = std::max(worstDir, (double)length(a.wi - U(b.wi)));
            worstL = std::max(worstL, maxRel3(b.L, a.L, 1e-9));
            worstPdf = std::max(worstPdf, relErr(b.pdf, a.pdf));
            ++samples;
        }
    }
    logf("  lights (city_night, %zu lights): %u points, choice equal %u, total %.2e rel, probability %.2e rel; %u samples: direction %.2e, L %.2e, pdf %.2e\n",
         cpu.count(), points, sameChoice, worstTotal, worstProb, samples, worstDir, worstL, worstPdf);
    CHECK(points > 1000);
    CHECK(sameChoice >= points - points / 1000);  // ties at the float boundary only
    CHECK(worstTotal < 1e-5 && worstProb < 1e-4 && worstDir < 1e-5 && worstL < 1e-5 && worstPdf < 1e-4);
}
} // namespace

int main()
{
    try
    {
        testSampler();
        testMaterial();
        testAtmosphere();
        testLights();
        if (g_failures)
        {
            std::printf("%d checks failed\n", g_failures);
            return 1;
        }
        std::printf("reference gpu shared-code tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}
