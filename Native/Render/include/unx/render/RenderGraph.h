#pragma once
#include "unx/render/Device.h"
#include "unx/render/GraphTypes.h"

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

class PassBuilder
{
public:
    TextureRef createTexture(const TextureDesc& desc);
    BufferRef createBuffer(const BufferDesc& desc);
    void use(TextureRef texture, Use use);
    void use(BufferRef buffer, Use use);
    // A shader-resource read of a buffer that writers declared after this pass need not wait for: the caller guarantees
    // that what those writers change is not what this pass reads (or does not change its result), so the read may overlap
    // them on another queue. It still waits for the writes before it (the barrier on its queue, the fence across queues).
    void useConcurrentRead(BufferRef buffer);
    // The pass has effects outside the graph (readback, persistent state) and is never culled.
    void keep();
    // The pass's command list ends right after it and is submitted with its own fence signal; once execute() has
    // submitted it, 'onSubmitted' receives the queue and the fence value that completes with this pass (and everything
    // before it on that queue). A CPU that waits for this pass's results (readbacks) then waits for this point of the
    // frame, not for the whole frame's queue. Implies keep(). Cost: one more ExecuteCommandLists on that queue.
    void fenceAfter(std::function<void(Queue& queue, uint64_t fence)> onSubmitted);

private:
    friend class RenderGraph;
    PassBuilder(class RenderGraph& graph, uint32_t pass) : m_graph(graph), m_pass(pass) {}
    RenderGraph& m_graph;
    uint32_t m_pass;
};

// One screen band of a banded pass group (RenderGraph::addBandedGroup): rows [y0, y1) of the view, starting on an 8-row
// tile boundary. A pass outside a group sees the whole view as band 0 of 1 (y1 = UINT32_MAX).
struct PassBand
{
    uint32_t index = 0, count = 1;
    uint32_t y0 = 0, y1 = UINT32_MAX;

    // The band 'rows' rows later (v1.31): [y0 - rows, y1 - rows) clamped at 0, the first band from row 0 and the last to
    // its end. For a pass that reads up to 'rows' rows below the row it writes (a 3 x 3 neighbourhood: 1; M's edge
    // detection and shading lag one 8-row tile row: 8): over a group's bands the lagged ranges still tile the view, and a
    // non-empty lagged range reads only rows this band or an earlier one produced (y1' + rows <= y1).
    PassBand lagged(uint32_t rows) const
    {
        PassBand b = *this;
        b.y0 = index == 0 ? 0 : (y0 > rows ? y0 - rows : 0);
        b.y1 = index + 1 >= count ? y1 : (y1 > rows ? y1 - rows : 0);
        return b;
    }
};

// Band 'index' of 'count' over 'height' rows exactly as addBandedGroup cuts them: y0 = min(height, floor(height * index /
// count) rounded down to 8 rows), the last band ends at 'height'. Tracks cut per-band work lists (tile lists, indirect
// arguments) with it before the group.
PassBand passBand(uint32_t height, uint32_t count, uint32_t index);

class PassContext
{
public:
    ID3D12GraphicsCommandList7* cmd = nullptr;
    QueueType queue = QueueType::Graphics;
    PassBand band;  // this pass's band (addBandedGroup); whole view otherwise
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
    uint32_t passScopes = 0;       // profiler scopes recorded (a timestamp each): live passes less those joined (joinPasses)
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
    const TextureDesc& desc(TextureRef t) const;  // at record time (e.g. a service choosing its pipeline by format)
    const BufferDesc& desc(BufferRef b) const;    // at record time (e.g. a buffer sized per frame by its producer)

    void addPass(std::string_view name, QueueType queue, const SetupFn& setup, ExecuteFn execute);
    // The number of passes added so far this frame (the index the next pass gets).
    uint32_t passCount() const;
    // Passes [first, first + count), added one after the other, are one profiler scope 'name': one timestamp and one
    // marker for the run, none between its passes. Nothing else changes - each pass keeps its declared uses, and the
    // barriers between them are the plan's (a pass that reads what the one before wrote waits for it exactly as two
    // separate passes do; two that touch different resources get no barrier and may overlap). Passes of the run that
    // are culled, or end up on another queue or command list, split the scope where they fall (PassChain.h).
    void joinPasses(uint32_t first, uint32_t count, std::string_view name);

    // A group of per-pixel passes recorded band by band (design revision 1, 4.8; INTERFACES 4): pass A on band 0, pass B
    // on band 0, ..., pass A on band 1, ..., so a band's intermediate data stays in L2 between producer and consumer
    // [measured, design bench --only-bands: resolve -> shade 0.740 -> 0.470 ms at 4K with 8 bands, UAV/SRV transitions
    // included]. Each pass is declared once per band with the same setup (the graph orders and synchronises each band's
    // passes like any other, one barrier batch per pass and band) and runs with PassContext::band set; it touches only
    // its band's rows. Reads across a band edge see finished rows of earlier bands only; a pass that needs the next
    // band's rows (3 x 3 neighbourhoods at the bottom edge) defers those rows to a later band itself.
    struct BandedPass
    {
        std::string name;  // pass names: "<group>.<name>.b<band>"
        QueueType queue = QueueType::Graphics;
        SetupFn setup;
        ExecuteFn execute;  // reads PassContext::band
    };
    void addBandedGroup(std::string_view group, uint32_t height, uint32_t bands, const std::vector<BandedPass>& passes);

    // Async compute policy. Off (default): every pass runs on the graphics queue and a frame is one command list in
    // one ExecuteCommandLists call. Measured on the RTX 4080 (Tests/Gates, --experiments): a cross-queue fence round
    // trip costs 62-80 us and each extra ExecuteCommandLists call ~15 us of GPU idle, while async overlap gains 2-6 %
    // (ARCHITECTURE 1.2, 4.3), so in-frame async compute only pays for large independent blocks and is opt-in.
    void setAsyncCompute(bool enabled) { m_asyncCompute = enabled; }
    bool asyncCompute() const { return m_asyncCompute; }
    // Selected large independent blocks on the async queue while setAsyncCompute is off: a pass declared for
    // QueueType::Compute whose name is listed ("name" exactly, or "prefix*") runs on the compute queue, every other pass
    // on the graphics queue. Applies to the passes added after the call. The data dependencies are the declared uses, so
    // the results are those of the one-queue frame; only the overlap and the cross-queue fences change the time.
    void setAsyncPasses(std::vector<std::string> names) { m_asyncPasses = std::move(names); }

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
    std::vector<std::string> m_asyncPasses;
    bool isAsyncPass(std::string_view name) const;
};
} // namespace unx::render
