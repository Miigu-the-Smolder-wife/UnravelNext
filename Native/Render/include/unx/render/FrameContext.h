#pragma once
// Per-frame inputs from the host (INTERFACES_KO.md 5.5). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/render/ViewDesc.h"

#include <cstdint>

namespace unx::render
{
struct FrameTiming;

// v1.50 (B6, Docs/Design/Requests/20260926_B_weather_fields.md): this tick's wind and weather from the World. The wind
// records are Passes/Atmosphere/WindField.hlsli WindRecord (80 B each, the World's evaluation order), their origins
// relative to windReference (the host subtracts it in double); time is the tick's World time (s). S builds the wind
// cache around the camera from them (FrameResources::wind). The weather values are the World's weather row.
struct WindFrame
{
    const void* records = nullptr;  // count x 80 B, valid until record() returns
    uint32_t count = 0;
    double time = 0;
    double reference[3] = { 0, 0, 0 };  // world position the record origins are relative to
};
struct WeatherFrame
{
    float rainRate = 0;     // mm/h
    float wetness = 0;      // 0..1, the World's dW/dt = rain - W / tau_dry
    float snowRate = 0;     // mm/h water equivalent
    float snowDepth = 0;    // m
    float fogDensity = 0;   // Mie scattering multiplier of the medium trajectory (0 = the scene's medium)
    float cloudCover = 0;   // 0..1
    float3 rainDirection{ 0, -1, 0 };  // unit, the direction the drops fall (the wind tilts it)
};

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
    // v1.51 (A5, COVERAGE 14.12 (2c)): the physical camera's lens for the aperture integral (depth of field): aperture
    // diameter (m; 0 = pinhole, no depth of field - the gate camera) and focus distance (m, along the view axis). The
    // circle of confusion of a depth z is f_px A |1/z - 1/z_focus| pixels (f_px = (H/2) proj[1][1]).
    float lensAperture = 0, lensFocus = 0;
    // v1.50 (A15, E): GPU pass timings of the last completed frame (GpuProfiler::lastCompleted), shown by the debug HUD
    // (quality key debug.hud); null = no timings (the HUD says so). The host keeps it valid until record() returns.
    const FrameTiming* timing = nullptr;
    CelestialFrame celestial;  // v1.49 (B4): moon, stars, airglow (S publishes FrameResources::celestial)
    WindFrame wind;            // v1.50 (B6): the World's wind records of this tick (S publishes FrameResources::wind)
    WeatherFrame weather;      // v1.50 (B6): rain, wetness, snow, fog, cloud cover
};
constexpr uint32_t kGpuSimulationSoft = 1, kGpuSimulationVfx = 2, kGpuSimulationRigid = 4;
constexpr uint32_t kDiscontinuityRestore = 1, kDiscontinuityCut = 2;
} // namespace unx::render
