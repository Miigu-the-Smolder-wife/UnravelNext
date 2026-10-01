// B8 GPU fluids through the host (HostRenderer::setFluids / UnxFrameSetFluids, FrameContext::fluids; engine 1's GPU
// bridge admission around each frame):
//   1. a physics-domain buffer reserved and bound on the renderer's bridge (as NativePhysics does in shared mode) and
//      handed over as an NP_FluidGpuView: every frame queued while it is set is admitted by the bridge and committed with
//      the frame's fence (one graphics submission per frame in the bridge's statistics), and frames without fluids admit
//      nothing;
//   2. an NP_FluidGpuView2 (136 B, version 2: an anchored domain's start origin and frame velocity) is accepted and
//      admitted; invalid views are refused on the calling thread (size, version 2 at 96 B, a non-finite start origin, stride
//      under 48 or not a multiple of 4, no particles, alpha
//      outside [0, 1], a material that is not Water-class, particle buffers of another device); the Water check follows
//      material edits after the commit (an appended Water material is accepted, one edited away from Water refused);
//   3. the lease and the buffer are released after the frames; no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "GpuBridge/GpuBridge.h"
#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <cstring>
#include <dxgi1_6.h>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
struct FluidGpuView  // NP_FluidGpuView (96 B)
{
    uint32_t size, version;
    void* current;
    void* start;
    uint64_t currentResource, startResource;
    uint32_t count, startCount, stride, startValid;
    double origin[3];
    float dx;
    uint32_t reserved;
    uint64_t tick;
};
static_assert(sizeof(FluidGpuView) == 96);
struct FluidGpuView2  // NP_FluidGpuView2 (136 B)
{
    FluidGpuView view;
    double startOrigin[3];
    float frameVelocity[3];
    uint32_t reserved;
};
static_assert(sizeof(FluidGpuView2) == 136);
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-96s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        auto throws = [](auto&& f) {
            try { f(); } catch (const std::exception&) { return true; }
            return false;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.debugLayer = true;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = host::test::oneBox();
        scene::Material water = h.scene().materials[0];
        water.cls = scene::MaterialClass::Water;
        water.ior = 1.33f;
        h.scene().materials.push_back(water);
        h.commit();
        uint64_t index = 0;
        auto frames = [&](uint32_t n) {
            for (uint32_t f = 0; f < n; ++f)
            {
                FramePacket p;
                p.frameIndex = index++;
                p.deltaTime = 1.0f / 60;
                p.width = 320;
                p.height = 180;
                p.camera = h.scene().cameras[0];
                h.renderStandalone(h.queueFrame(std::move(p)), nullptr, 0);
            }
        };
        const uint32_t particles = 4096;
        ComPtr<ID3D12Resource> buffer;
        {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = (uint64_t)particles * 80;  // the physics GPU particle (NP_FluidGpuView::stride)
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(h.d3dDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)), "fluid buffer");
        }
        NRC_GpuBridge bridge = h.gpuBridge().acquire();
        uint64_t id = 0;
        {
            NRC_GpuAllocation a{};
            a.size = sizeof a;
            a.version = 1;
            const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
            a.bytes = h.d3dDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            a.owner = 1;
            a.domain = NRC_GPU_PHYSICS_DOMAIN;
            a.memory = NRC_GPU_PERSISTENT;
            if (bridge.reserve(bridge.context, &a, &id) != NRC_GPU_OK || !id) fail("reserve");
            if (bridge.bind(bridge.context, id, buffer.Get(), D3D12_RESOURCE_STATE_COMMON) != NRC_GPU_OK) fail("bind");
        }
        FluidGpuView view{};
        view.size = sizeof view;
        view.version = 1;
        view.current = buffer.Get();
        view.currentResource = id;
        view.count = particles;
        view.stride = 80;
        view.dx = 0.05f;
        view.origin[0] = 1, view.origin[1] = 2, view.origin[2] = 3;
        view.tick = 42;
        const uint64_t stamp[6] = { 7, 1, 1, 42, 1, 1 };
        auto submissions = [&] {
            NRC_GpuStatistics s = h.gpuBridge().statistics();
            return s.submissions[0];
        };

        // 1.
        const uint64_t before = submissions();
        HostRenderer::FluidInput in;
        in.view = &view;
        in.alpha = 0.5f;
        in.domainCells[0] = in.domainCells[1] = in.domainCells[2] = 64;
        in.material = 1;
        h.setFluids({ &in, 1 }, stamp);
        frames(3);
        const uint64_t with = submissions();
        h.setFluids({}, stamp);
        frames(2);
        const uint64_t after = submissions();
        logf("  graphics submissions on the bridge: %llu before, %llu after 3 frames with the fluid, %llu after 2 without\n", (unsigned long long)before,
             (unsigned long long)with, (unsigned long long)after);
        expect("each frame with the fluid is admitted and committed once", with == before + 3);
        expect("frames without fluids admit nothing", after == with);

        // 2.
        FluidGpuView bad = view;
        bad.size = 64;
        HostRenderer::FluidInput badIn = in;
        badIn.view = &bad;
        expect("a view of another size is refused", throws([&] { h.setFluids({ &badIn, 1 }, stamp); }));
        {
            FluidGpuView2 anchored{};
            anchored.view = view;
            anchored.view.size = sizeof anchored;
            anchored.view.version = 2;
            anchored.startOrigin[0] = 0.6, anchored.startOrigin[1] = 2, anchored.startOrigin[2] = 3;
            anchored.frameVelocity[0] = 24;
            HostRenderer::FluidInput anchoredIn = in;
            anchoredIn.view = &anchored;
            const uint64_t v2Before = submissions();
            expect("an NP_FluidGpuView2 (version 2, 136 B) is accepted", !throws([&] { h.setFluids({ &anchoredIn, 1 }, stamp); }));
            frames(1);
            expect("its frame is admitted and committed once", submissions() == v2Before + 1);
            h.setFluids({}, stamp);
            FluidGpuView2 short2 = anchored;
            short2.view.size = sizeof(FluidGpuView);
            anchoredIn.view = &short2;
            expect("version 2 at the version 1 size is refused", throws([&] { h.setFluids({ &anchoredIn, 1 }, stamp); }));
            FluidGpuView2 nan2 = anchored;
            nan2.startOrigin[1] = std::nan("");
            anchoredIn.view = &nan2;
            expect("a non-finite start origin is refused", throws([&] { h.setFluids({ &anchoredIn, 1 }, stamp); }));
        }
        bad = view;
        bad.stride = 32;
        expect("a stride under 48 is refused", throws([&] { h.setFluids({ &badIn, 1 }, stamp); }));
        bad.stride = 82;
        expect("a stride that is not a multiple of 4 is refused", throws([&] { h.setFluids({ &badIn, 1 }, stamp); }));
        bad = view;
        bad.count = 0;
        expect("a view without particles is refused", throws([&] { h.setFluids({ &badIn, 1 }, stamp); }));
        HostRenderer::FluidInput badAlpha = in;
        badAlpha.alpha = 1.5f;
        expect("alpha outside [0, 1] is refused", throws([&] { h.setFluids({ &badAlpha, 1 }, stamp); }));
        {
            // a buffer of another device (the software adapter's): the physics ran on another renderer's bridge
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            ComPtr<ID3D12Device> other;
            check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&other)), "WARP device");
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = buffer->GetDesc();
            ComPtr<ID3D12Resource> foreign;
            check(other->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&foreign)), "foreign buffer");
            FluidGpuView elsewhere = view;
            elsewhere.current = foreign.Get();
            HostRenderer::FluidInput foreignIn = in;
            foreignIn.view = &elsewhere;
            expect("particle buffers of another device are refused", throws([&] { h.setFluids({ &foreignIn, 1 }, stamp); }));
        }
        HostRenderer::FluidInput badMaterial = in;
        badMaterial.material = 0;
        expect("a material that is not Water-class is refused", throws([&] { h.setFluids({ &badMaterial, 1 }, stamp); }));
        // Materials edited after the commit (engine 1's D0 finding): a Water material appended by an edit is accepted, the
        // committed Water material edited to Standard is refused, and a committed Standard one stays refused.
        {
            const uint32_t appended = (uint32_t)h.scene().materials.size(), committedWater = appended - 1;
            scene::Material water2 = h.scene().materials[committedWater];
            scene::Material standard = h.scene().materials[0];
            const std::pair<uint32_t, scene::Material> add[1] = { { appended, water2 } };
            h.editMaterials(add);
            HostRenderer::FluidInput edited = in;
            edited.material = appended;
            expect("a Water material appended by an edit is accepted", !throws([&] { h.setFluids({ &edited, 1 }, stamp); }));
            frames(1);
            const std::pair<uint32_t, scene::Material> change[1] = { { committedWater, standard } };
            h.editMaterials(change);
            HostRenderer::FluidInput changed = in;
            changed.material = committedWater;
            expect("a Water material edited to Standard is refused", throws([&] { h.setFluids({ &changed, 1 }, stamp); }));
            expect("a committed Standard material stays refused", throws([&] { h.setFluids({ &badMaterial, 1 }, stamp); }));
            h.setFluids({}, stamp);
            frames(1);
        }

        // 3.
        bridge.release_resource(bridge.context, id);
        bridge.release(bridge.context);
        frames(2);
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST FLUIDS TEST FAILED (%u)\n" : "HOST FLUIDS TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST FLUIDS TEST ERROR: %s\n", e.what());
        return 2;
    }
}
