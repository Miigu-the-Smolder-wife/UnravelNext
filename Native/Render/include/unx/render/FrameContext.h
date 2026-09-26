#pragma once
// Per-frame inputs from the host (INTERFACES_KO.md 5.5). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/render/ViewDesc.h"

#include <cstdint>

namespace unx::render
{
struct FrameTiming;

// v1.49 (B4, FEATURES_GAME 11): the sky's celestial objects of this frame, set by the host from its time and place
// (Passes/Atmosphere/Celestial.h: sky::celestial, sky::directionalLight, sky::celestialFrame). flags: bit 0 the frame's
// directional light (Scene::sun) is the moon (the sky pass leaves out its uniform solar disk), bit 1 the moon's disk is
// drawn (a Lambert sphere lit by sunDirection), bit 2 the stars are drawn. 0 (the default): none of them, as before.
struct CelestialFrame
{
    float3 moonDirection{ 0, -1, 0 };
    float moonAngularRadius = 0.00452f;
    float3 sunDirection{ 0, 1, 0 };  // the true sun (it lights the moon)
    float sunIlluminance = 128000;   // lux at the top of the atmosphere
    float3 sunColor{ 1, 1, 1 };
    float moonAlbedo = 0.12f;
    float3x4 equatorialToWorld;      // J2000 equatorial -> world rotation (3 x 3 part)
    float airglowRadiance = 0;       // nits at the zenith (natural night sky ~2e-4); 0 = none
    uint32_t flags = 0;
};

struct FrameContext
{
    uint64_t frameIndex = 0;
    double time = 0;
    float deltaTime = 0;
    ViewDesc mainView;
    // Validation runs: the main view's colour is linear radiance x exposure in RGBA32F (metrics, INTERFACES 9)
    // instead of the display-encoded RGB10A2.
    bool outputLinearHdr = false;
    // GPU simulation steps submitted in this frame (v1.35, design revision 10.3): kGpuSimulation* bits, 0 = none (the
    // default). R's GI spreads its per-frame ray mean by it (fewer rays in frames that carry a simulation step).
    uint32_t gpuSimulation = 0;
    // History discontinuity of this frame (v1.35, I request 20260925_I_history_discontinuity.md), set by the host for
    // the first frame after the event:
    //   kDiscontinuityRestore: World snapshot restore, save load, branch change -- every temporal state resets
    //                          (world-space caches included), and no instance has motion in this frame;
    //   kDiscontinuityCut:     camera cut -- view-bound histories reset, world-space caches (R's GI cache) are kept.
    // Either bit: the main view has no previous view (prevViewProj = viewProj). Instance teleports are per instance
    // (InstanceTransformUpdate::flags).
    uint32_t discontinuity = 0;
    // v1.45 (A4, FEATURES_GAME 6.2): automatic exposure. The renderer meters the main view's luminance and sets
    // mainView.ev100 each frame (M, Exposure.cpp: one frame of meter latency, exact adaptation over deltaTime; cuts and
    // restores snap); the host's mainView.ev100 is the starting value. exposureCompensation in stops (+1 = brighter).
    bool autoExposure = false;
    float exposureCompensation = 0;
    // v1.46 (A4, FEATURES_GAME 6): HDR display output. 0: SDR (the main view's colour is R10G10B10A2, sRGB-encoded after
    // the tone curve). >= 1: the display's peak over paper-white luminance; the colour is R16G16B16A16 FLOAT holding
    // display-referred linear Rec.709 light, 1 = paper white, after the tone curve generalised to that peak (M, Post.cpp;
    // at 1 exactly the SDR curve); the host encodes it for the swap chain.
    float displayPeak = 0;
    // v1.50 (A15, E): GPU pass timings of the last completed frame (GpuProfiler::lastCompleted), shown by the debug HUD
    // (quality key debug.hud); null = no timings (the HUD says so). The host keeps it valid until record() returns.
    const FrameTiming* timing = nullptr;
    CelestialFrame celestial;  // v1.49 (B4): moon, stars, airglow (S publishes FrameResources::celestial)
};
constexpr uint32_t kGpuSimulationSoft = 1, kGpuSimulationVfx = 2, kGpuSimulationRigid = 4;
constexpr uint32_t kDiscontinuityRestore = 1, kDiscontinuityCut = 2;
} // namespace unx::render
