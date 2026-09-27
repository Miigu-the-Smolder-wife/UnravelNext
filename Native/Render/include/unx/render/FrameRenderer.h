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
    // Photo mode (B11, FEATURES_GAME 17; render A): the frame shows 'image', an exposed-linear image made elsewhere (the
    // GPU reference tracer's progressive image), through M's post chain into 'output' (the display encoding of
    // frame.displayPeak) instead of rendering the scene; 'sdrCopy' (optional, RGB10A2) also gets the chain's SDR
    // encoding (a saved PNG). The GPU scene still takes the frame's updates, so record() continues where it left off.
    void recordImage(RenderGraph& graph, const FrameContext& frame, TextureRef image, TextureRef output, TextureRef sdrCopy = {});
    // Persistent track state (tests and gates read track statistics through it, e.g. unx::visibility::latestStats).
    TrackState& trackState() { return m_trackState; }
    // The main view's EV100 of the last recorded frame (automatic exposure's choice when on; tests, statistics).
    float lastEv100() const { return m_lastEv100; }
    // The b1 constants of a view (what every view's frameConstants slot holds); for tests that build their own context.
    static gpu::FrameConstants frameConstants(const GpuScene& scene, const FrameContext& frame, const ViewDesc& view);

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
    float m_lastEv100 = 0;
    // Temporal upscale (FrameContext::Upscale): the previous frame's jittered and unjittered view-projections, exposure
    // and output size; valid after the first upscaled frame.
    float4x4 m_upscalePrevJittered{}, m_upscalePrevViewProj{};
    float m_upscalePrevExposure = 0, m_upscalePrevJitterX = 0, m_upscalePrevJitterY = 0;
    uint32_t m_upscalePrevWidth = 0, m_upscalePrevHeight = 0;
    bool m_upscaleValid = false;
    void setupUpscale(FrameContext& frame);
    uint32_t m_debugDraw = 0xFFFFFFFFu;  // this frame's FrameConstants::debugDraw (tracks::debugBegin)
    float m_viewModelScale = 1.0f;       // this frame's FrameConstants::viewModelScale (tracks::viewModelPrepare)
};
} // namespace unx::render
