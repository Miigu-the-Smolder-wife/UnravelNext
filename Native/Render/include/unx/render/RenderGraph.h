#pragma once
#include "unx/render/Device.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace unx::render
{
class GpuProfiler;

// How a pass touches a resource. Write uses (Uav*, RenderTarget, DepthWrite, CopyDst, AccelerationStructureWrite,
// AccelerationStructureScratch) are read-modify-write.
// UavComputeDisjoint: the pass writes a region no other pass of the same run touches (tile-classified per-class
// material/shading dispatches, per-cascade ranges). Consecutive disjoint writers of a resource need no barrier between
// them; the first one still waits for earlier accesses and the next other access waits for all of them.
// SrvGraphics/UavGraphics synchronise with every shader stage (D3D12_BARRIER_SYNC_ALL_SHADING), which includes ray
// tracing shaders: DispatchRays passes declare their textures and buffers with them.
// AccelerationStructure*: buffers only, no views (the owner creates the AS SRV). The result buffers are created by
// their owner with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE and imported; a pass may declare Write and
// Read of the same AS buffer (in-place refit).
enum class Use : uint8_t
{
    SrvCompute,
    SrvGraphics,
    UavCompute,
    UavComputeDisjoint,
    UavGraphics,
    RenderTarget,
    DepthWrite,
    DepthRead,
    IndirectArgs,
    CopySrc,
    CopyDst,
    AccelerationStructureWrite,    // build / refit destination (BLAS, TLAS)
    AccelerationStructureRead,     // traversal (DispatchRays, RayQuery), BLAS read by a TLAS build, refit source
    AccelerationStructureInput,    // build inputs: deformed vertices, index buffers, instance descriptors
    AccelerationStructureScratch,  // build scratch (transients get ALLOW_UNORDERED_ACCESS)
};

struct TextureDesc
{
    const char* name = "";
    uint32_t width = 1;
    uint32_t height = 1;
    uint16_t depthOrArraySize = 1;
    uint16_t mipLevels = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // View formats other than 'format' (INTERFACES_KO.md 4, v1.13): the texture is created castable to them (relaxed
    // format casting, same bits per texel), e.g. written as R32_UINT through its UAV and filtered as
    // R9G9B9E5_SHAREDEXP through its SRV. UNKNOWN = 'format'.
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
};

struct BufferDesc
{
    const char* name = "";
    uint64_t size = 0;
    uint32_t stride = 0;  // 0 = raw (ByteAddressBuffer) views
};

struct TextureRef
{
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
struct BufferRef
{
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};

class PassBuilder
{
public:
    TextureRef createTexture(const TextureDesc& desc);
    BufferRef createBuffer(const BufferDesc& desc);
    void use(TextureRef texture, Use use);
    void use(BufferRef buffer, Use use);
    // The pass has effects outside the graph (readback, persistent state) and is never culled.
    void keep();

private:
    friend class RenderGraph;
    PassBuilder(class RenderGraph& graph, uint32_t pass) : m_graph(graph), m_pass(pass) {}
    RenderGraph& m_graph;
    uint32_t m_pass;
};

class PassContext
{
public:
    ID3D12GraphicsCommandList7* cmd = nullptr;
    QueueType queue = QueueType::Graphics;
    uint32_t srv(TextureRef t) const;
    uint32_t uav(TextureRef t) const;
    uint32_t srv(BufferRef b) const;
    uint32_t uav(BufferRef b) const;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(TextureRef t) const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(TextureRef t) const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvReadOnly(TextureRef t) const;  // for Use::DepthRead
    ID3D12Resource* resource(TextureRef t) const;
    ID3D12Resource* resource(BufferRef b) const;
    D3D12_GPU_VIRTUAL_ADDRESS address(BufferRef b) const;
    const TextureDesc& desc(TextureRef t) const;
    // Root constants b0 (up to Device::kRootConstantCount dwords) for the pipeline type bound next.
    void computeConstants(const void* data, uint32_t dwords) const;
    void graphicsConstants(const void* data, uint32_t dwords) const;
    // Root CBV b1 (Frame.hlsli) for the compute and, on the graphics queue, graphics pipelines.
    void bindFrameConstants(D3D12_GPU_VIRTUAL_ADDRESS address) const;

private:
    friend class RenderGraph;
    const class RenderGraph* m_graph = nullptr;
};

struct RenderGraphStats
{
    uint32_t declaredPasses = 0;
    uint32_t livePasses = 0;
    uint32_t transientResources = 0;
    uint32_t barrierBatches = 0;   // Barrier() calls
    uint32_t barriers = 0;         // individual texture/buffer barriers
    uint32_t crossQueueSyncs = 0;  // fence waits inserted between queues
    uint32_t commandLists = 0;
    uint64_t transientBytesAliased = 0;  // heap size actually used
    uint64_t transientBytesUnaliased = 0;  // sum of resource sizes
    bool planReused = false;
    double cpuCompileMs = 0;  // plan build (0 when reused)
    double cpuRecordMs = 0;
    double cpuSubmitMs = 0;
};

// Frame render graph (ARCHITECTURE_KO.md 7.1-2). Passes are declared every frame; the compiled plan (culling, order,
// aliasing, placed resources, views, barrier lists, queue synchronisation) is cached by a structural key and reused
// while the frame structure is unchanged. Barriers are D3D12 enhanced barriers, batched per pass, emitted only on
// real hazards or layout changes. Transient resources used only on the graphics queue alias one heap by lifetime;
// resources that touch the async compute queue get their own memory (no cross-queue alias hazards).
class RenderGraph
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(PassContext&)>;

    explicit RenderGraph(Device& device);
    ~RenderGraph();

    // Imported resources keep their memory across frames. 'layout' is the texture layout at frame start and the
    // layout the graph leaves it in at frame end (buffers: ignored).
    TextureRef importTexture(ID3D12Resource* resource, const TextureDesc& desc, D3D12_BARRIER_LAYOUT layout);
    BufferRef importBuffer(ID3D12Resource* resource, const BufferDesc& desc);
    // Transient resources: memory and views come from the graph; the lifetime spans the first to the last live use.
    TextureRef createTexture(const TextureDesc& desc);
    BufferRef createBuffer(const BufferDesc& desc);

    void addPass(std::string_view name, QueueType queue, const SetupFn& setup, ExecuteFn execute);

    // Async compute policy. Off (default): every pass runs on the graphics queue and a frame is one command list in
    // one ExecuteCommandLists call. Measured on the RTX 4080 (Tests/Gates, --experiments): a cross-queue fence round
    // trip costs 62-80 us and each extra ExecuteCommandLists call ~15 us of GPU idle, while async overlap gains 2-6 %
    // (ARCHITECTURE 1.2, 4.3), so in-frame async compute only pays for large independent blocks and is opt-in.
    void setAsyncCompute(bool enabled) { m_asyncCompute = enabled; }
    bool asyncCompute() const { return m_asyncCompute; }

    // Compiles (or reuses) the plan, records all passes, submits them with the queue synchronisation, and resets
    // the declarations for the next frame. 'profiler' (optional) timestamps every pass.
    void execute(GpuProfiler* profiler);
    const RenderGraphStats& stats() const { return m_stats; }
    // Last fence value submitted on each queue by execute().
    uint64_t lastFence(QueueType q) const { return m_lastFence[(size_t)q]; }
    // Tests: whether two transients of the last executed frame (refs from that frame) occupy overlapping heap memory.
    bool sharesMemory(TextureRef a, TextureRef b) const { return sharesMemory(a.id, b.id); }
    bool sharesMemory(BufferRef a, BufferRef b) const { return sharesMemory(a.id, b.id); }
    bool sharesMemory(uint32_t a, uint32_t b) const;  // resource ids

private:
    friend class PassBuilder;
    friend class PassContext;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Device& m_device;
    RenderGraphStats m_stats;
    uint64_t m_lastFence[kQueueTypeCount] = {};
    bool m_asyncCompute = false;
};
} // namespace unx::render
