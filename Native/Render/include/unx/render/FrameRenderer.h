#pragma once
// Frame orchestration (INTERFACES_KO.md 5.2). Owner: core. Calls the track entry points (Tracks.h) in the order of
// ARCHITECTURE 4.1 on one graphics queue, provides FrameServices, and allocates per-view frame constants.
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"

namespace unx::render
{
class FrameRenderer
{
public:
    static constexpr uint32_t kMaxViewsPerFrame = 16;

    // framesInFlight must match the caller's frame pacing: frame constants of frame f live in slot f % framesInFlight
    // and the caller waits for that slot's fences before recording frame f (as Harness does).
    FrameRenderer(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, GpuScene& scene, uint32_t framesInFlight = 2);
    ~FrameRenderer();

    // Declares the whole frame into 'graph'; 'output' is the main view's colour target (imported by the caller:
    // RGB10A2 display output, or RGBA32F linear radiance when frame.outputLinearHdr). Returns the main view products.
    ViewResources record(RenderGraph& graph, const FrameContext& frame, TextureRef output);

private:
    D3D12_GPU_VIRTUAL_ADDRESS allocateFrameConstants(const FrameContext& frame, const ViewDesc& view);
    Device& m_device;
    ShaderLibrary& m_shaders;
    const QualityConfig& m_quality;
    GpuScene& m_scene;
    uint32_t m_framesInFlight;
    ComPtr<ID3D12Resource> m_constants;
    uint8_t* m_mapped = nullptr;
    uint32_t m_slotViews = 0;
    uint64_t m_slotFrame = UINT64_MAX;
    TrackState m_trackState;
};
} // namespace unx::render
