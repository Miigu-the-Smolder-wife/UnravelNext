// Track W water shading correctness (WaterShading.hlsli; GPU probe against a double-precision reference):
//   - Fresnel from air and from water at 181 angles (0..90 degrees) equals the exact unpolarised dielectric Fresnel
//     equations (|d| <= 2e-6); normal incidence R0 = ((n - 1) / (n + 1))^2
//   - total internal reflection from water exactly past the critical angle asin(1 / 1.33), and no refraction there
//   - refracted directions obey Snell (n1 sin_i = n2 sin_t, |d| <= 2e-6) and are unit length
//   - reciprocity: R from air at theta_i equals R from water at the refracted angle theta_t (Stokes, |d| <= 2e-6)
//   - energy: R + T = 1 at every angle by construction (T = 1 - R), and R, T in [0, 1]
//   unx_test_water_watershadingtests [--no-debug-layer]
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
double fresnel(double cosI, double n1, double n2)
{
    const double sinT = n1 / n2 * std::sqrt(std::max(0.0, 1 - cosI * cosI));
    if (sinT >= 1) return 1;
    const double cosT = std::sqrt(1 - sinT * sinT);
    const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT), rp = (n2 * cosI - n1 * cosT) / (n2 * cosI + n1 * cosT);
    return 0.5 * (rs * rs + rp * rp);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
        Device device([&] { DeviceOptions o; o.debugLayer = debugLayer; return o; }());
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        constexpr uint32_t kAngles = 181;
        D3D12_HEAP_PROPERTIES rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kAngles * 48;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device.d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)), "readback");
        RenderGraph g(device);
        ID3D12PipelineState* pso = shaders.compute("Passes/Water/Tests/WaterShadingProbe");
        const BufferRef out = g.createBuffer({ "water probe", kAngles * 48, 0 });
        g.addPass("water probe", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(out, Use::UavCompute); },
                  [=](PassContext& c) { const uint32_t k[4] = { c.uav(out), kAngles, 0, 0 }; c.cmd->SetPipelineState(pso); c.computeConstants(k, 4); c.cmd->Dispatch((kAngles + 63) / 64, 1, 1); });
        ID3D12Resource* rb = readback.Get();
        g.addPass("water probe read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(out, Use::CopySrc); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(rb, 0, c.resource(out), 0, kAngles * 48); });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        const float* r = nullptr;
        check(readback->Map(0, nullptr, (void**)&r), "map probe");
        const double n = 1.33, critical = std::asin(1 / n);
        double worstR = 0, worstSnell = 0, worstReciprocity = 0;
        for (uint32_t i = 0; i < kAngles; ++i)
        {
            const float* p = r + 12 * i;
            const double cosI = p[8], sinI = p[9];  // the incidence the GPU used (its sin/cos approximations)
            const double airR = fresnel(cosI, 1, n), waterR = fresnel(cosI, n, 1);
            worstR = std::max({ worstR, std::abs(p[0] - airR), std::abs(p[1] - waterR) });
            W_CHECK(p[0] >= 0 && p[0] <= 1 && p[1] >= 0 && p[1] <= 1, "angle %u: R out of [0, 1]", i);
            // From air: always refracts; Snell sin_t = sin_i / n on the tangent axis.
            W_CHECK(p[4] == 1, "angle %u: no refraction from air", i);
            worstSnell = std::max({ worstSnell, std::abs(-p[2] - sinI / n), std::abs(std::sqrt(double(p[2]) * p[2] + double(p[3]) * p[3]) - 1) });
            // Reciprocity: R from air at theta equals R from water at theta_t, with theta_t from the same cos the Fresnel
            // term used (the GPU's float sin and cos are not exactly on the unit circle, which matters near grazing).
            const double cosT = std::sqrt(1 - (1 - cosI * cosI) / (n * n));
            worstReciprocity = std::max(worstReciprocity, std::abs(p[0] - fresnel(cosT, n, 1)));
            // From water: total internal reflection exactly past the critical angle.
            const float e = (float)n, c = (float)cosI; const bool tir = e * e * (1.0f - c * c) >= 1.0f;  // the kernel's float test
            W_CHECK((p[7] == 0) == tir && (!tir || p[1] == 1.0f), "angle %u (%.1f deg, critical %.3f): TIR %d, refracted %g, R %g", i, 0.5 * i, critical * 180 / 3.14159265358979323846, tir, p[7], p[1]);
            if (!tir) worstSnell = std::max(worstSnell, std::abs(-p[5] - sinI * n));
        }
        const double r0 = std::pow((n - 1) / (n + 1), 2);
        W_CHECK(std::abs(r[0] - r0) <= 2e-6, "normal incidence R %g, expected %g", r[0], r0);
        readback->Unmap(0, nullptr);
        W_CHECK(worstR <= 2e-6 && worstSnell <= 2e-6 && worstReciprocity <= 2e-6, "Fresnel %.3g, Snell %.3g, reciprocity %.3g", worstR, worstSnell, worstReciprocity);
        const uint32_t errors = device.drainDebugMessages();
        W_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("water shading: %u angles; Fresnel vs exact %.2e, Snell %.2e, reciprocity %.2e; R0 %.6f; TIR past %.3f deg\n", kAngles, worstR, worstSnell, worstReciprocity, r0,
                    critical * 180 / 3.14159265358979323846);
        std::printf("water shading tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
