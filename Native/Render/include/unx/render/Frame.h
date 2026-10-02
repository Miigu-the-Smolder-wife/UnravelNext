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

// The froxel grid's tile size (pixels) of a frame - S's lists and air volume, E's media, the atmosphere record all take
// it from here. atmosphere.froxels.tile_px holds up to main views tile_reference_height pixels high; a higher main view
// gets tiles larger in proportion (the same angle per tile: the grid's content varies with direction, not per pixel).
// mainHeight: the frame's main view (every view of a frame shares the tile size: the atmosphere record has one).
inline uint32_t froxelTilePx(const QualityConfig& q, uint32_t mainHeight)
{
    const uint32_t base = (uint32_t)q.integer("atmosphere.froxels.tile_px");
    const uint32_t reference = q.has("atmosphere.froxels.tile_reference_height") ? (uint32_t)q.integer("atmosphere.froxels.tile_reference_height") : 0u;
    if (reference == 0 || mainHeight <= reference) return base;
    return (uint32_t)(((uint64_t)base * mainHeight + reference / 2) / reference);
}

struct FramePassContext;

// A pass whose kernel reads the main view's air (atmosphereAerial / atmosphereAirView: Passes/Atmosphere/Atmosphere.hlsli)
// reads the fog's volume with it (FogVolume.hlsli fogAt, through the frame constants): it declares these beside the air.
inline void declareFog(PassBuilder& b, const FrameResources& r, Use use)
{
    if (r.fogVolume.valid()) b.use(r.fogVolume, use);
    for (const TextureRef& t : r.fogSecondary)  // (a planar view's pass reads its own; declaring the others costs nothing)
        if (t.valid()) b.use(t, use);
}

// Every resource a reader of the mesh-card surface cache touches (FrameResources::cards), as shader resources of 'use'
// (SrvCompute, or SrvGraphics for DispatchRays passes).
inline void declareSurfaceCacheCards(PassBuilder& b, const SurfaceCacheCardRefs& r, Use use)
{
    if (!r.valid()) return;
    b.use(r.frame, use);
    b.use(r.instanceMap, use);
    b.use(r.meshCards, use);
    b.use(r.cards, use);
    b.use(r.cardPages, use);
    b.use(r.pageTable, use);
    b.use(r.depth, use);
    b.use(r.albedo, use);
    b.use(r.normal, use);
    b.use(r.emissive, use);
    b.use(r.direct, use);
    b.use(r.indirect, use);
    b.use(r.final, use);
}

// The indirect-light source of the passes that light air, particles, water and glass (Passes/GI/GiSource.hlsli): the
// Lumen translucency volume when this frame has published one (tracks::globalIllumination), else the world GI cache,
// else none. Taken where the pass is recorded - a pass recorded before GI gets none.
struct GiSource
{
    BufferRef cache;
    TextureRef ambient, directional;
    uint32_t params = 0xFFFFFFFFu;
    bool valid() const { return params != 0xFFFFFFFFu || cache.valid(); }
};
inline GiSource giSource(const FrameResources& r)
{
    GiSource s;
    if (r.translucencyGiParams != 0xFFFFFFFFu && r.translucencyGiAmbient.valid() && r.translucencyGiDirectional.valid())
    {
        s.ambient = r.translucencyGiAmbient;
        s.directional = r.translucencyGiDirectional;
        s.params = r.translucencyGiParams;
    }
    else
        s.cache = r.giCache;
    return s;
}
inline void declareGiSource(PassBuilder& b, const GiSource& s, Use use)
{
    if (s.params != 0xFFFFFFFFu)
    {
        b.use(s.ambient, use);
        b.use(s.directional, use);
    }
    else if (s.cache.valid())
        b.use(s.cache, use);
}
// The word the pass hands its kernel (GiSource.hlsli).
inline uint32_t giSourceWord(PassContext& c, const GiSource& s)
{
    return s.params != 0xFFFFFFFFu ? (s.params | 0x80000000u) : s.cache.valid() ? c.srv(s.cache) : 0xFFFFFFFFu;
}

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
