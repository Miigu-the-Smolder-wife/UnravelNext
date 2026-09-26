#pragma once
// Per-frame inputs from the host (INTERFACES_KO.md 5.5). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/render/ViewDesc.h"

#include <cstdint>

namespace unx::render
{
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
};
constexpr uint32_t kGpuSimulationSoft = 1, kGpuSimulationVfx = 2, kGpuSimulationRigid = 4;
constexpr uint32_t kDiscontinuityRestore = 1, kDiscontinuityCut = 2;
} // namespace unx::render
