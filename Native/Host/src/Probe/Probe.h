#pragma once
// Host boundary probe (ARCHITECTURE 7.1-5, I track). Decides how the renderer meets the Unity host by measuring the
// same frame workload through each boundary:
//   FusedList          host work + frame + host work in one command list, one ExecuteCommandLists (lower bound)
//   HostQueueLists     the frame is its own list on the host's queue, between the host's lists (Unity:
//                      IUnityGraphicsD3D12v8::ExecuteCommandList on Unity's queue)
//   OwnQueue           the frame runs on the renderer's own DIRECT queue of the host's device; fences hand the output
//                      to the host queue and back (Unity: device shared, queue not)
//   IndependentDevice  the frame runs on a separate device (ID3D12DeviceFactory); the output texture and the fences are
//                      shared through NT handles (Unity: nothing shared but the adapter)
// The workload is self-contained (its own root signature, heap and kernels on any ID3D12Device), so the same code runs
// in the standalone gate and inside Unity's process on Unity's device.
#include "unx/render/D3D12.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace unx::host::probe
{
using render::ComPtr;

enum class Mode : uint32_t
{
    FusedList = 0,
    HostQueueLists = 1,
    OwnQueue = 2,
    IndependentDevice = 3,
};
const char* modeName(Mode mode);
constexpr uint32_t kModeCount = 4;

struct WorkDesc
{
    uint32_t width = 3840, height = 2160;
    uint32_t heavyPasses = 18;   // full-screen RGBA16F read + write each (~0.22 ms at 4K [expected])
    uint32_t tinyPasses = 102;   // dependent Dispatch(1) passes: 18 + 102 + output = 121, the P0b graph's pass count
};

// Root signature, shader-visible heap and pipelines of the probe on one device. The root signature has the engine's
// layout (unx::render::Device: 32 root constants at b0, root CBV b1, static samplers s0-s4, directly indexed heaps) so
// the kernels include the engine's Bindless.hlsli unchanged.
class ProbeGpu
{
public:
    ProbeGpu(ID3D12Device* device, const std::filesystem::path& shaderDirectory);
    ID3D12Device* device() const { return m_device.Get(); }
    ID3D12RootSignature* rootSignature() const { return m_rootSignature.Get(); }
    ID3D12DescriptorHeap* heap() const { return m_heap.Get(); }
    ID3D12PipelineState* heavy() const { return m_heavy.Get(); }
    ID3D12PipelineState* tiny() const { return m_tiny.Get(); }
    ID3D12PipelineState* output() const { return m_output.Get(); }
    ID3D12PipelineState* present() const { return m_present.Get(); }
    // Creates a UAV in the next free heap slot and returns its bindless index.
    uint32_t uav(ID3D12Resource* resource, DXGI_FORMAT format);
    uint32_t rawUav(ID3D12Resource* buffer, uint32_t bytes);
    void bind(ID3D12GraphicsCommandList* cmd) const;

private:
    ComPtr<ID3D12PipelineState> load(const std::filesystem::path& file);
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_descriptorSize = 0, m_next = 0;
    ComPtr<ID3D12PipelineState> m_heavy, m_tiny, m_output, m_present;
};

// The frame workload: heavyPasses full-screen ping-pong passes and tinyPasses dependent single-group passes in
// alternation, then the output pass that writes RGB10A2 into the host-visible output. Every pass is separated from
// the next by a global UAV barrier (enhanced barriers), which is what the render graph emits between dependent passes.
class ProbeWork
{
public:
    ProbeWork(ProbeGpu& gpu, const WorkDesc& desc);
    const WorkDesc& desc() const { return m_desc; }
    // Records the passes; 'outputUav' is the bindless UAV (this ProbeGpu's heap) of an RGB10A2 texture in the
    // UNORDERED_ACCESS layout/state.
    void record(ID3D12GraphicsCommandList7* cmd, uint32_t outputUav) const;

private:
    ProbeGpu& m_gpu;
    WorkDesc m_desc;
    ComPtr<ID3D12Resource> m_ping, m_pong, m_counter;
    uint32_t m_pingUav = 0, m_pongUav = 0, m_counterUav = 0;
};

// Committed texture helper; 'flags' adds D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS always.
ComPtr<ID3D12Resource> createTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                     D3D12_RESOURCE_STATES state, D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE,
                                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, const wchar_t* name = nullptr);
void globalUavBarrier(ID3D12GraphicsCommandList7* cmd);

// Timestamps of one queue mapped to the CPU clock (QueryPerformanceCounter ticks), so spans on different queues and
// devices can be compared. Calibrated once per run; drift over a run of ~10 s is far below the reported quantities.
struct QueueClock
{
    uint64_t gpuFrequency = 0, gpuAtCalibration = 0, cpuAtCalibration = 0, cpuFrequency = 0;
    void calibrate(ID3D12CommandQueue* queue);
    double toMs(uint64_t gpuTicks) const;  // CPU-clock milliseconds since an arbitrary origin
};

// Device-level facts the boundary decision needs (read on whichever device the host hands over).
struct DeviceFacts
{
    std::string adapter;
    uint64_t adapterLuid = 0;
    std::string runtimePath, runtimeVersion;  // D3D12Core.dll loaded in this process
    std::string shaderModel;
    uint32_t meshShaderTier = 0, raytracingTier = 0, bindingTier = 0, heapTier = 0;
    bool enhancedBarriers = false;
    bool typedUavLoadAdditionalFormats = false;
    std::string toJson() const;
};
DeviceFacts readDeviceFacts(ID3D12Device* device);
std::string jsonEscape(const std::string& s);
} // namespace unx::host::probe
