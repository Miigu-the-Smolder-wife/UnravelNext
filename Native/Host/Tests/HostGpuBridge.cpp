// Engine 1's GPU bridge wired into the host renderer (84922cc; UnxAcquireGpuBridge / UnxReleaseGpuBridge /
// UnxGpuBridgeStatistics):
//   1. every renderer has one bridge on its own device: the leased table names that device, its generation is nonzero and
//      differs between two renderers (fences never compare across devices);
//   2. a lease is given back through the table's own release, and the statistics report the device's generation;
//   3. frames render while leases exist and after they end; destroying the renderer quiesces the bridge first (no
//      debug-layer errors, no hang).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "GpuBridge/GpuBridge.h"
#include "TestScenes.h"
#include "unx/core/File.h"

#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;

int main()
{
    try
    {
        uint32_t failures = 0, errors = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-96s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        auto make = [] {
            HostRendererOptions o;
            o.standalone = true;
            o.framesInFlight = 2;
            o.debugLayer = true;
            o.shaderDirectory = executableDirectory() / "shaders";
            o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
            auto r = std::make_unique<HostRenderer>(o);
            r->scene() = host::test::oneBox();
            r->commit();
            return r;
        };
        auto frames = [](HostRenderer& r, uint32_t n) {
            for (uint32_t f = 0; f < n; ++f)
            {
                FramePacket p;
                p.frameIndex = f;
                p.deltaTime = 1.0f / 60;
                p.width = 320;
                p.height = 180;
                p.camera = r.scene().cameras[0];
                r.renderStandalone(r.queueFrame(std::move(p)), nullptr, 0);
            }
        };
        {
            auto a = make();
            auto b = make();
            NRC_GpuBridge la = a->gpuBridge().acquire(), lb = b->gpuBridge().acquire();
            logf("  leases: size %u version %u generation %llu / %llu, device %p / %p (renderers %p / %p)\n", la.size, la.version,
                 (unsigned long long)la.generation, (unsigned long long)lb.generation, la.device, lb.device, (void*)a->d3dDevice(), (void*)b->d3dDevice());
            expect("the lease is a 136 B table on the renderer's own device", la.size == sizeof(NRC_GpuBridge) && la.device == a->d3dDevice() && lb.device == b->d3dDevice());
            expect("each renderer's bridge has its own nonzero generation", la.generation != 0 && lb.generation != 0 && la.generation != lb.generation);
            frames(*a, 4);  // frames while a lease exists
            NRC_GpuStatistics s = a->gpuBridge().statistics();
            expect("statistics report the bridge's generation", s.generation == la.generation);
            la.release(la.context);
            lb.release(lb.context);
            frames(*a, 4);
            errors += a->debugErrors() + b->debugErrors();
        }  // destroyed: the bridge quiesces before the device goes
        expect("D3D12 debug layer errors 0 (and the renderers were destroyed)", errors == 0);
        logf(failures ? "HOST GPU BRIDGE TEST FAILED (%u)\n" : "HOST GPU BRIDGE TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST GPU BRIDGE TEST ERROR: %s\n", e.what());
        return 2;
    }
}
