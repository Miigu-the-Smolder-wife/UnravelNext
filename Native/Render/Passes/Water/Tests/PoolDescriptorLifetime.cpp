#include "unx/water/Pool.h"
#include "unx/water/RoundPool.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include <memory>
using namespace unx;
using namespace unx::render;

// Keep a real submitted graphics fence incomplete while destroying a basin.
// Its descriptors must remain live, then return to the heap after completion.
template<class Basin, class Description>
void checkLifetime(Device& device, ShaderLibrary& shaders, const char* name)
{
    device.waitIdle(); device.collectGarbage();
    const uint32_t baseline = device.descriptors().resourcesInUse();
    auto basin = std::make_unique<Basin>(device, shaders, Description{});
    const uint32_t allocated = device.descriptors().resourcesInUse();
    if (allocated <= baseline) fail("%s did not allocate source descriptors", name);
    ComPtr<ID3D12Fence> gate;
    check(device.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "test completion gate");
    struct ReleaseGate { ID3D12Fence* fence; ~ReleaseGate() { fence->Signal(1); } } release{gate.Get()};
    auto& queue = device.queue(QueueType::Graphics);
    check(queue.get()->Wait(gate.Get(), 1), "hold submitted frame");
    auto commands = device.acquireCommandList(QueueType::Graphics);
    const uint64_t fence = device.submit(commands);
    basin.reset(); device.collectGarbage();
    if (device.descriptors().resourcesInUse() != allocated) fail("%s recycled descriptors before the GPU completed", name);
    check(gate->Signal(1), "release submitted frame"); queue.waitCpu(fence); device.collectGarbage();
    if (device.descriptors().resourcesInUse() != baseline) fail("%s retained descriptors after GPU completion", name);
    logf("%s: %u source descriptors held until real GPU fence completion, then reclaimed\n", name, allocated - baseline);
}
int main()
{
    try
    {
        DeviceOptions options; options.debugLayer = true; options.gpuValidation = true;
        Device device(options); ShaderLibrary shaders(device, executableDirectory() / "shaders");
        checkLifetime<water::Pool, water::PoolDesc>(device, shaders, "rectangular pool");
        checkLifetime<water::RoundPool, water::RoundPoolDesc>(device, shaders, "round pool");
        if (device.drainDebugMessages()) fail("D3D12 validation reported an error");
        return 0;
    }
    catch (const std::exception& e) { logf("FAIL: %s\n", e.what()); return 1; }
}
