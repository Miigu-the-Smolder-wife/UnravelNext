#pragma once
// Frame contract between the render tracks (INTERFACES_KO.md 5). Core owns this file; tracks read it and fill the
// resources they own. Adding a field or service goes through the interface-change procedure (INTERFACES_KO.md 0).
// v1.42 (infra request 20260926_Infra_header_split): the contracts that change most live in their own headers --
// ViewDesc.h, FrameResources.h (ViewResources, FrameResources), FrameContext.h, DepthRaster.h, GraphTypes.h, ViewKind.h --
// and a file that needs only those includes them instead of this one. This header keeps the frame context, services and
// track state, and for now still includes everything it did before (GpuSceneLayout.h, RenderGraph.h, Shaders.h, ...), so
// no file needs to change at once.
#include "unx/core/Config.h"
#include "unx/core/Math.h"
#include "unx/render/DepthRaster.h"
#include "unx/render/Device.h"
#include "unx/render/FrameContext.h"
#include "unx/render/FrameResources.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/ViewDesc.h"
#include "unx/scene/SceneData.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unx::render
{
class GpuScene;

struct FramePassContext;

// Bands of a banded pass group for a view of width x height (RenderGraph::addBandedGroup): the view's pixels over
// output.band_pixels (quality key, core; 4K / 8 = L2-sized intermediates), at least 1; band_pixels = 0 is one band (the
// default since v1.31: M measured a net loss with bands while shading is latency-bound).
uint32_t passBandCount(const QualityConfig& quality, uint32_t width, uint32_t height);

// Persistent state of a track (history buffers, pools, caches) owned by the FrameRenderer: created on first use,
// destroyed with the renderer after the GPU is idle. Keys are "<track>.<name>"; one key always holds one type.
class TrackState
{
public:
    template <typename T>
    T& get(const std::string& key)
    {
        std::shared_ptr<void>& p = m_entries[key];
        if (!p) p = std::make_shared<T>();
        return *static_cast<T*>(p.get());
    }
    void clear() { m_entries.clear(); }
    // One recording of a frame (FrameRenderer::record): tracks that count per-frame allocations key them on this as well
    // as on the frame index - a recording that failed keeps its frame index, and the next attempt starts from zero.
    void beginRecord() { ++m_record; }
    uint64_t recordSerial() const { return m_record; }

private:
    std::unordered_map<std::string, std::shared_ptr<void>> m_entries;
    uint64_t m_record = 0;
};

// Cross-track services, provided by core (unx_frame) so modules never link each other.
struct FrameServices
{
    std::function<void(FramePassContext&, const DepthRasterRequest&)> rasterizeDepth;  // [V]
    // Records V -> M(resolve) -> S(shadow visibility) -> M(shading) for a secondary view and returns its products;
    // color is linear radiance x exposure in RGBA16F, sized view.width x view.height. Used by R for planar mirrors.
    std::function<ViewResources(FramePassContext&, const ViewDesc&)> renderView;
    // v1.73: called by V inside a view's coverage passes after its rasters and before the count, so another track can
    // append coverage records (W's ocean edges: ViewResources::oceanEdgePixels, coverageAppend). Empty = none.
    std::function<void(FramePassContext&, const ViewResources&)> coverageAppend;
    // R-W2 / R-2 (render B's request, 2026-09-27; format agreed with engine 1): traces refraction rays for W's water and
    // M's glass composite after they write the job list and before they composite. jobs (raw): a 16 B head { count,
    // indirect dispatch x, y, z } then 48 B per job { float3 origin, uint outputSlot; float3 direction, uint flags (bits
    // 0..7 medium, 8..9 total internal reflections left, 31 coverage record); float3 sigmaA, float iorInside }, at most
    // maxJobs; results: 8 B per output slot (RGBA16F, linear radiance x exposure, absorption included). Filled by the
    // renderer from tracks::refraction; empty (a build without R) = the caller keeps its own path.
    std::function<void(FramePassContext&, BufferRef jobs, BufferRef results, uint32_t maxJobs)> traceRefractions;
};

struct FramePassContext
{
    Device& device;
    RenderGraph& graph;
    ShaderLibrary& shaders;
    const QualityConfig& quality;
    GpuScene& scene;
    const FrameContext& frame;
    FrameResources& resources;
    FrameServices& services;
    // Frame constants for a view (allocates a 1 KB slot of this frame): bind with PassContext::bindFrameConstants.
    std::function<D3D12_GPU_VIRTUAL_ADDRESS(const ViewDesc&)> frameConstantsFor;
    TrackState* trackState = nullptr;  // FrameRenderer's; null in contexts built without a renderer
    uint32_t framesInFlight = 2;       // per-frame CPU-written resources of frame f live in slot f % framesInFlight;
                                       // the caller waits for that slot's fences before recording frame f

    template <typename T>
    T& state(const std::string& key)
    {
        if (!trackState) fail("FramePassContext::state('%s'): no track state in this context", key.c_str());
        return trackState->get<T>(key);
    }
};
} // namespace unx::render
