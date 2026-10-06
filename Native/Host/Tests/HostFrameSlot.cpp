// Real unfinished GPU work must remain owned while the coordinator polls.
#include "Renderer/HostRenderer.h"
#include "TestScenes.h"
#include "unx/core/File.h"
#include <chrono>
#include <thread>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

int main()
{
    try
    {
        DeviceOptions deviceOptions; deviceOptions.debugLayer = true;
        Device device(deviceOptions);
        auto* queue = device.queue(QueueType::Graphics).get();
        HostRendererOptions options;
        options.standalone = false; options.hostDevice = device.d3d(); options.hostQueue = queue; options.framesInFlight = 1;
        options.shaderDirectory = executableDirectory() / "shaders";
        options.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer host(options); host.scene() = test::oneBox(); host.commit();
        if (!host.frameSlotReady()) fail("fresh frame slot is not ready");
        ComPtr<ID3D12Resource> output;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 320; desc.Height = 180;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&output)), "slot test output");
        auto render = [&](uint64_t index) {
            FramePacket frame; frame.frameIndex = index; frame.deltaTime = 1.f / 60;
            frame.width = 320; frame.height = 180; frame.camera = host.scene().cameras[0]; frame.output = output.Get();
            host.renderOnHost(host.queueFrame(std::move(frame)), [queue](ID3D12CommandList* list, ID3D12Resource*) {
                ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
            });
        };
        // Initial lazy renderer uploads may synchronously wait on this queue.
        // Finish them before holding a normal in-flight frame behind the gate.
        for (uint64_t index = 0; index < 3; ++index)
        {
            render(index);
            auto warm = std::chrono::steady_clock::now();
            while (!host.frameSlotReady())
            {
                if (std::chrono::steady_clock::now() - warm > std::chrono::seconds(15)) fail("warm-up did not complete");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ComPtr<ID3D12Fence> gate;
        check(device.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "slot test gate");
        // Declared after host: release the gate before host teardown on failure.
        struct ReleaseGate { ID3D12Fence* fence; ~ReleaseGate() { fence->Signal(1); } } release{gate.Get()};
        check(queue->Wait(gate.Get(), 1), "hold frame slot");
        render(3);
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            if (host.frameSlotReady()) fail("poll retired an unfinished GPU frame slot");
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(1)) fail("readiness poll blocked");
        check(gate->Signal(1), "release frame slot");
        while (!host.frameSlotReady())
        {
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(15)) fail("completed slot did not become ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (device.drainDebugMessages() || host.debugErrors()) fail("frame-slot debug-layer error");
        logf("HOST FRAME SLOT TEST PASS: 100 nonblocking polls retained an unfinished frame; ready after its real fence.\n");
        return 0;
    }
    catch (const std::exception& error) { logf("HOST FRAME SLOT TEST ERROR: %s\n", error.what()); return 1; }
}
