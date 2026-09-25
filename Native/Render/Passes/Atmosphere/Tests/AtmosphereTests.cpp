// S atmosphere correctness (no GPU lock): every LUT and every public lookup against the double-precision reference
// (AtmosphereReference.h), quadrature convergence of the configured counts, determinism, and the rebuild policy.
//   unx_test_atmosphere_atmospheretests [--no-debug-layer]
#include "TestFrame.h"

#include "AtmosphereReference.h"
#include "AtmosphereSystem.h"

#include <algorithm>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;
namespace ref = unx::render::atmosphere::reference;

namespace
{
constexpr double kPi = 3.14159265358979323846;

ref::D3 d3(float3 v) { return { v.x, v.y, v.z }; }
ref::D3 d3(float4 v) { return { v.x, v.y, v.z }; }

struct Lut
{
    std::vector<uint8_t> data;
    uint32_t width = 0, height = 0;
    float4 at(uint32_t x, uint32_t y, uint32_t z = 0) const { return texel(data, width, height, x, y, z); }
};

// C++ twin of airMultipleScattering (AtmosphereCommon.hlsli): bilinear on the GPU LUT.
ref::D3 psiFromLut(const Lut& ms, const atmosphere::AtmosphereParams& p, const ref::Model& m, ref::D3 pos, ref::D3 sun)
{
    const double alt = std::clamp(ref::altitudeOf(m, pos) / (m.top - m.bottom), 0.0, 1.0);
    const double qx = (ref::dot(ref::upOf(m, pos), sun) * 0.5 + 0.5) * (p.multiScatterSize[0] - 1), qy = alt * (p.multiScatterSize[1] - 1);
    const uint32_t x0 = std::min((uint32_t)qx, p.multiScatterSize[0] - 1), y0 = std::min((uint32_t)qy, p.multiScatterSize[1] - 1);
    const uint32_t x1 = std::min(x0 + 1, p.multiScatterSize[0] - 1), y1 = std::min(y0 + 1, p.multiScatterSize[1] - 1);
    const double fx = qx - x0, fy = qy - y0;
    auto v = [&](uint32_t x, uint32_t y) { return d3(ms.at(x, y)); };
    return (v(x0, y0) * (1 - fx) + v(x1, y0) * fx) * (1 - fy) + (v(x0, y1) * (1 - fx) + v(x1, y1) * fx) * fy;
}

double relErr(ref::D3 a, ref::D3 b, double floor)
{
    double e = 0;
    for (int k = 0; k < 3; ++k) e = std::max(e, std::abs((&a.x)[k] - (&b.x)[k]) / std::max(std::abs((&b.x)[k]), floor));
    return e;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
        TestFrame tf(debugLayer);

        scene::Scene sc;
        sc.name = "atmosphere test";
        sc.sun.direction = normalize(float3{ 0.55f, 0.25f, 0.2f });  // low sun: long paths, strong horizon gradients
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 3, 1.7f, -2 };
        cam.forward = normalize(float3{ 0.8f, 0.05f, 0.3f });
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 3840, 2160, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;

        const atmosphere::AtmosphereParams p = atmosphere::makeParams(sc.atmosphere, tf.quality);
        const ref::Model m = ref::fromScene(sc.atmosphere);
        const ref::D3 sun = ref::normalize(d3(sc.sun.direction));
        const ref::D3 camPos = d3(cam.position);

        // Queries for the public lookups.
        std::vector<float4> skyQ, sunQ;
        for (double el : { -0.6, -0.08, -0.02, -0.004, 0.0, 0.003, 0.02, 0.1, 0.4, 1.2 })
            for (double az : { 0.0, 0.05, 0.4, 1.3, 2.4, 3.1 })
            {
                // Azimuth relative to the sun's horizontal direction.
                const double sa = std::atan2(sun.z, sun.x) + az;
                skyQ.push_back({ (float)(std::cos(el) * std::cos(sa)), (float)std::sin(el), (float)(std::cos(el) * std::sin(sa)), 0 });
            }
        for (float3 pos : { float3{ 0, 0, 0 }, float3{ 100, 30, -50 }, float3{ -2000, 800, 1500 } }) sunQ.push_back({ pos.x, pos.y, pos.z, 0 });

        std::shared_ptr<std::vector<uint8_t>> rt, rm, rs, oSky, oSun;
        auto probe = [&](FramePassContext& fc, int mode, const std::vector<float4>& q) {
            const uint32_t n = (uint32_t)q.size(), outCount = mode == 2 ? 2 * n : n;
            BufferRef in = tf.uploadBuffer(fc, q.data(), q.size() * 16, 16, "probe queries");
            BufferRef out = fc.graph.createBuffer(BufferDesc{ "probe out", outCount * 16ull, 16 });
            ID3D12PipelineState* pso = fc.shaders.compute(format("Passes/Atmosphere/Tests/AtmosphereProbe.MODE%d", mode));
            const FrameResources r = fc.resources;
            const D3D12_GPU_VIRTUAL_ADDRESS cb = fc.frameConstantsFor(fc.frame.mainView);
            fc.graph.addPass("s.test.probe", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 for (TextureRef t : { r.transmittanceLut, r.multiScatterLut, r.skyViewLut }) b.use(t, Use::SrvCompute);
                                 b.use(in, Use::SrvCompute);
                                 b.use(out, Use::UavCompute);
                             },
                             [=](PassContext& c) {
                                 const uint32_t k[8] = { c.srv(r.transmittanceLut), c.srv(r.multiScatterLut), c.srv(r.skyViewLut), 0xFFFFFFFFu, c.srv(in), c.uav(out), n, 0 };
                                 c.cmd->SetPipelineState(pso);
                                 c.bindFrameConstants(cb);
                                 c.computeConstants(k, 8);
                                 c.cmd->Dispatch((n + 63) / 64, 1, 1);
                             });
            return tf.readbackBuffer(fc, out, outCount * 16ull);
        };

        tf.run([&](FramePassContext& fc) {
            tracks::atmosphere(fc);
            rt = tf.readback(fc, fc.resources.transmittanceLut);
            rm = tf.readback(fc, fc.resources.multiScatterLut);
            rs = tf.readback(fc, fc.resources.skyViewLut);
            oSky = probe(fc, 0, skyQ);
            oSun = probe(fc, 1, sunQ);
        });
        const Lut T{ *rt, p.transmittanceSize[0], p.transmittanceSize[1] };
        const Lut MS{ *rm, p.multiScatterSize[0], p.multiScatterSize[1] + 1 };
        const Lut SKY{ *rs, p.skyViewSize[0], p.skyViewSize[1] };
        int failures = 0;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("%-58s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        // 1. Transmittance LUT against the independent double integral at texel centres (same parameterisation).
        {
            double maxT = 0, maxTau = 0;
            for (uint32_t y : { 0u, 1u, 7u, 16u, 32u, 50u, 63u })
                for (uint32_t x : { 0u, 3u, 16u, 64u, 128u, 200u, 250u, 255u })
                {
                    const double bottom = m.bottom, top = m.top, H = std::sqrt((top - bottom) * (top + bottom));
                    const double rho = double(y) / (p.transmittanceSize[1] - 1) * H;
                    const double alt = rho * rho / (std::sqrt(rho * rho + bottom * bottom) + bottom), r = bottom + alt;
                    const double dmin = top - r, dmax = rho + H, d = dmin + double(x) / (p.transmittanceSize[0] - 1) * (dmax - dmin);
                    const double mu = d <= 0 ? 1 : std::clamp((H * H - rho * rho - d * d) / (2 * r * d), -1.0, 1.0);
                    const ref::D3 tau = ref::opticalDepth(m, alt, mu, 16384);
                    const float4 g = T.at(x, y);
                    for (int k = 0; k < 3; ++k)
                    {
                        const double tr = (&tau.x)[k], tg = (&g.x)[k];
                        const double eT = std::abs(std::exp(-tg) - std::exp(-tr));
                        if (eT > 2e-5) logf("  T texel (%u,%u) ch%d alt %.3f mu %.6f: gpu tau %.6e ref %.6e\n", x, y, k, alt, mu, tg, tr);
                        maxT = std::max(maxT, eT);
                        if (std::exp(-tr) > 1e-4) maxTau = std::max(maxTau, std::abs(tg - tr) / std::max(tr, 1e-6));
                    }
                }
            report(maxT < 2e-5, "transmittance LUT: max |T - T_ref| (texel centres)", maxT, 2e-5);
            report(maxTau < 1e-4, "transmittance LUT: max rel. optical depth error (T > 1e-4)", maxTau, 1e-4);
        }

        // 2. Multiple-scattering LUT: GPU implementation vs the same formula in double, and the configured quadrature
        //    against a 4x finer one (convergence of atmosphere.multiscatter_directions / _steps).
        {
            double impl = 0, conv = 0;
            for (uint32_t y : { 0u, 5u, 20u })
                for (uint32_t x : { 3u, 16u, 29u })  // sun 54 deg below, at, and 60 deg above the horizon
                {
                    const double h = double(y) / (p.multiScatterSize[1] - 1) * (m.top - m.bottom);
                    const double mus = double(x) / (p.multiScatterSize[0] - 1) * 2 - 1;
                    const ref::D3 a = ref::multiScatter(m, h, mus, (int)p.multiScatterDirections, (int)p.multiScatterSteps);
                    const ref::D3 b = ref::multiScatter(m, h, mus, 4 * (int)p.multiScatterDirections, 4 * (int)p.multiScatterSteps);
                    impl = std::max(impl, relErr(d3(MS.at(x, y)), a, 1e-7));
                    conv = std::max(conv, relErr(a, b, 1e-7));
                    if (relErr(a, b, 1e-7) > 2e-3) logf("  MS texel (%u,%u) h %.0f mus %.3f: configured %.4e %.4e %.4e, 4x %.4e %.4e %.4e\n", x, y, h, mus, a.x, a.y, a.z, b.x, b.y, b.z);
                }
            report(impl < 2e-3, "multi-scatter LUT: GPU vs double, same quadrature (rel.)", impl, 2e-3);
            report(conv < 1e-2, "multi-scatter LUT: configured vs 4x quadrature (rel.)", conv, 1e-2);
        }

        // 3. Sky radiance through the public lookup (interpolated, arbitrary directions) vs the reference ray integral
        //    with exact sun transmittance and the GPU Psi_ms.
        {
            double worst = 0, worstAbove = 0;
            const double E = sc.sun.illuminance;
            for (size_t i = 0; i < skyQ.size(); ++i)
            {
                float4 g;
                std::memcpy(&g, oSky->data() + i * 16, 16);
                const ref::D3 d = ref::normalize(d3(skyQ[i]));
                const ref::D3 r = ref::skyRadiance(m, camPos, d, sun, [&](ref::D3 q, ref::D3 s) { return psiFromLut(MS, p, m, q, s); }, 4096);
                const ref::D3 gpu = d3(g) * (1.0 / E);
                const double e = relErr(gpu, r, 1e-7);
                worst = std::max(worst, e);
                if (d.y > 0.01) worstAbove = std::max(worstAbove, e);
                if (e > 5e-3) logf("  sky dir (%.4f %.4f %.4f): gpu %.6e ref %.6e rel %.2e\n", d.x, d.y, d.z, gpu.y, r.y, e);
            }
            report(worstAbove < 5e-3, "sky lookup vs reference, elevation > 0.6 deg (rel.)", worstAbove, 5e-3);
            report(worst < 2e-2, "sky lookup vs reference, all incl. horizon band (rel.)", worst, 2e-2);
        }

        // 4. Sun disk radiance.
        {
            double worst = 0;
            const double sinT = std::sin((double)sc.sun.angularRadius);
            for (size_t i = 0; i < sunQ.size(); ++i)
            {
                float4 g;
                std::memcpy(&g, oSun->data() + i * 16, 16);
                const ref::D3 Tr = ref::sunTransmittance(m, d3(sunQ[i]), sun, 16384);
                const ref::D3 r = Tr * (sc.sun.illuminance / (kPi * sinT * sinT));
                worst = std::max(worst, relErr(d3(g), r, 1e-3));
            }
            // Bilinear interpolation of the 256 x 64 optical-depth LUT: ~3.5e-4 measured (0.035 %, below 10-bit output).
            report(worst < 5e-4, "sun disk radiance vs reference (rel.)", worst, 5e-4);
        }

        // 5. Aerial perspective: the air volume is built by froxels() on the froxel grid (it reads the VSM); its accuracy
        //    against this reference is checked in Passes/Shadow/Tests/FroxelTests.cpp.

        // 6. Rebuild policy and determinism: same inputs -> no passes; changed sun -> sky view only, same bits
        //    when the original sun comes back.
        {
            const atmosphere::AtmosphereStats before = atmosphere::stats(tf.trackState);
            tf.run([&](FramePassContext& fc) { tracks::atmosphere(fc); });
            const atmosphere::AtmosphereStats same = atmosphere::stats(tf.trackState);
            report(same.lutBuilds == before.lutBuilds && same.skyViewBuilds == before.skyViewBuilds,
                   "unchanged inputs rebuild nothing (builds added)", double(same.skyViewBuilds - before.skyViewBuilds), 0);
            scene::Scene moved = sc;
            moved.sun.direction = normalize(float3{ 0.2f, 0.8f, 0.1f });
            tf.setScene(moved);
            tf.run([&](FramePassContext& fc) { tracks::atmosphere(fc); });
            tf.setScene(sc);
            std::shared_ptr<std::vector<uint8_t>> rs2;
            tf.run([&](FramePassContext& fc) {
                tracks::atmosphere(fc);
                rs2 = tf.readback(fc, fc.resources.skyViewLut);
            });
            const atmosphere::AtmosphereStats after = atmosphere::stats(tf.trackState);
            report(after.lutBuilds == before.lutBuilds, "sun change keeps transmittance/multi-scatter (builds)", double(after.lutBuilds - before.lutBuilds), 0);
            report(*rs2 == *rs, "sky view deterministic across rebuilds (bit-exact)", *rs2 == *rs ? 0.0 : 1.0, 0);
        }

        const uint32_t debugErrors = tf.device.drainDebugMessages();
        report(debugErrors == 0, "D3D12 debug layer errors", debugErrors, 0);
        logf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
