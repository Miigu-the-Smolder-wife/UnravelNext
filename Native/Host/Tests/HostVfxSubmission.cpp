// World publication must remain independent of a render frame waiting for its GPU slot.
#include "Renderer/HostRenderer.h"
#include "../../Render/Passes/FX/Tests/RppStream.h"
#include "TestScenes.h"
#include "unx/core/File.h"
#include <chrono>
#include <cstring>
#include <future>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;
using namespace std::chrono_literals;

int main()
{
    try
    {
        DeviceOptions deviceOptions; deviceOptions.debugLayer = true;
        Device owner(deviceOptions);
        Queue& graphics = owner.queue(QueueType::Graphics);
        HostRendererOptions options;
        options.standalone = false;
        options.hostDevice = owner.d3d(); options.hostQueue = graphics.get();
        options.framesInFlight = 1;
        options.shaderDirectory = executableDirectory() / "shaders";
        options.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer host(options);
        host.scene() = test::oneBox();
        host.commit();
        ComPtr<ID3D12Resource> output;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 320; desc.Height = 180; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(owner.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
              nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&output)), "submission output");
        ComPtr<ID3D12Fence> gate;
        check(owner.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "submission gate");
        struct OpenGate { ID3D12Fence* value; ~OpenGate() { value->Signal(1); } } open{ gate.Get() };
        uint64_t frame = 0;
        auto packet = [&] {
            FramePacket p;
            p.frameIndex = frame++; p.time = frame / 60.0; p.deltaTime = 1.0f / 60;
            p.width = 320; p.height = 180; p.camera = host.scene().cameras[0]; p.output = output.Get();
            return host.queueFrame(std::move(p));
        };
        auto execute = [&](ID3D12CommandList* list, ID3D12Resource*) {
            graphics.get()->ExecuteCommandLists(1, &list);
        };
        for (int i = 0; i < 4; ++i) { host.renderOnHost(packet(), execute); graphics.waitCpu(graphics.signal()); }
        // Block a submitted frame on the GPU, after recording is complete. The
        // next rendering call must wait for its sole frame slot, not block the producer.
        bool armed = false;
        host.renderOnHost(packet(), [&](ID3D12CommandList* list, ID3D12Resource* target) {
            if (!armed) { check(graphics.get()->Wait(gate.Get(), 1), "hold previous frame"); armed = true; }
            execute(list, target);
        });
        const uint64_t next = packet();
        auto rendering = std::async(std::launch::async, [&] { host.renderOnHost(next, execute); });
        bool renderBlocked = false, submittedWithoutGpu = false;
        fx::test::RppConfig config;
        config.emitters = 1; config.particles = 32; config.bodies = 0; config.fields = 0;
        config.features = config.heightfield = config.sheet = config.turbulence = config.mesh = false;
        fx::test::RppStream stream(config);
        std::vector<uint8_t> bytes = stream.next(nullptr);
        NV_StreamHeader header{}; std::memcpy(&header, bytes.data(), sizeof header);
        try
        {
            renderBlocked = rendering.wait_for(50ms) == std::future_status::timeout;
            auto producer = std::async(std::launch::async, [&] { host.vfxSubmit(bytes.data(), bytes.size()); });
            submittedWithoutGpu = producer.wait_for(500ms) == std::future_status::ready;
            check(gate->Signal(1), "unblock render frame");
            producer.get(); rendering.get();
        }
        catch (...) { gate->Signal(1); rendering.wait(); throw; }
        if (!renderBlocked || !submittedWithoutGpu) fail("World submission blocked on the render GPU slot");
        const auto readback = host.vfxReadback(header.stream, header.generation, header.tick);
        if (readback.counters.status != 0) fail("admitted packet failed: 0x%x", readback.counters.status);
        if (host.debugErrors() || owner.drainDebugMessages()) fail("submission test D3D12 error");
        logf("PASS World VFX packet admitted while render frame slot was GPU-blocked; tick consumed once; D3D12 errors 0\n");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
