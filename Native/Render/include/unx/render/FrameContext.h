#pragma once
// Per-frame inputs from the host (INTERFACES_KO.md 5.5). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/render/ViewDesc.h"

#include <cstdint>
#include <vector>

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

// B8 (engine 1 W, INTERFACES v1.70): one GPU fluid of this frame - the physics module's particles on the renderer's device
// (shared mode, NP_FluidGpuView), read by W's waterGeometry. The host admitted the reads through the GPU bridge
// (GpuBridgeHost::prepareGraphics) before the frame's lists run and commits them with the frame's fence.
struct FluidFrame
{
    ID3D12Resource* current = nullptr;  // particles at the end of the latest tick (the physics GPU particle: FluidGpu.hlsl Particle, 'stride' B each; COMMON)
    ID3D12Resource* start = nullptr;    // particles at the start of that tick (null or !startValid: no blend)
    uint32_t count = 0, startCount = 0, stride = 80, startValid = 0;  // stride: NP_FluidGpuView::stride (>= 48; 80 today)
    double origin[3] = {};              // world position of cell 0 in this frame's coordinates (origin shifts applied)
    double startOrigin[3] = {};         // cell 0 of the start buffer's positions (NP_FluidGpuView2: an anchored domain where
                                        // the tick started; = origin when not anchored)
    float frameVelocity[3] = {};        // m/s the particles' velocities are relative to (the anchored domain's; 0 otherwise)
    float dx = 0;                       // cell size (m); particle positions are in cells
    float alpha = 1;                    // this frame's time between the tick's start (0) and end (1)
    uint64_t tick = 0;
    uint32_t domainCells[3] = {};       // the fluid domain (FluidSurfaceDesc::nodes = 2 x cells)
    uint32_t material = 0;              // the scene material of its surface (Water class)
};

// B7 (engine 1 W, INTERFACES v1.72): the frame's sea - unx::water::OceanDesc's spectrum, the still water level and the
// water body - read by W's waterGeometry (FFT, view grid) into FrameResources::oceanDepth / waterSurface.
struct OceanFrame
{
    float windSpeed = 0, windDirection = 0, fetch = 0, spread = 0;  // unx::water::OceanDesc
    uint32_t seed = 0;
    float level = 0;                          // still water height (this frame's coordinates: origin shifts applied)
    float horizontalBound = 0, verticalBound = 0;  // the roughest sea's R and A (m), used until the sea is measured
    uint32_t lake = 0;                        // water body: 0 the open sea, 1 a circular lake
    float lakeCentre[2] = {}, lakeRadius = 0; // (x, z) in this frame's coordinates, m
};

// W2 (engine 2 W, INTERFACES v1.78; FEATURES_GAME 1.10): a closed basin (bath, pool) of this frame, read by W's
// waterGeometry (unx::water::Pool: exact basin-mode ripples, a layer-1 triangle stream). id is stable across frames (the
// basin's simulation state; a basin absent from a frame's list is released); a basin out of every view and without
// sources is not evolved in that frame and catches up exactly when drawn again. Sources are this frame's only (the host
// hands each one to exactly one frame), in this frame's coordinates.
struct PoolSourceFrame
{
    double x = 0, z = 0;     // centre (m), inside the basin
    float radius = 0.05f;    // Gaussian footprint sigma (m)
    float impulse = 0;       // vertical impulse on the water (N s, positive = pushed down)
    float volume = 0;        // change of the displaced volume there (m^3, positive = water pushed out)
};
struct PoolFrame
{
    uint32_t id = 0;              // nonzero, unique in the frame
    uint32_t material = 0;        // scene material of the surface (M's Water class)
    uint32_t shape = 0;           // v1.92 (W2-R): 0 rectangle; 1 round (sizeX = the diameter, sizeZ unused; RoundPool)
    float sizeX = 0, sizeZ = 0;   // inner basin (m): walls at local 0 and size
    float depth = 0;              // uniform water depth (m); 0 = deep water
    float surfaceFilm = 0;        // 0: a clean surface; 1: an inextensible film (bathers, soap)
    double centre[3] = {};        // the still surface's centre (y = the level with no bodies), this frame's coordinates
    float yaw = 0;                // about +y (rad): local x -> (cos, 0, -sin), local z -> (sin, 0, cos)
    const PoolSourceFrame* sources = nullptr;
    uint32_t sourceCount = 0;
};

// B5 (render B, INTERFACES v1.77): the frame's cloud layer - weather content (an environment input, not a quality key).
// coverage 0 = no cloud pass. Altitudes in world metres (S keeps the layer fixed to the world across origin shifts with
// FrameContext::originShift); the wind advects in FrameContext::time (World time).
struct CloudLayerDesc
{
    float coverage = 0;                          // [0, 1]
    float baseAltitude = 1500, topAltitude = 4000;  // m
    float sigmaMax = 0.04f;                      // peak extinction (1/m)
    float albedo = 0.99f;                        // single-scattering albedo
    float windX = 0, windZ = 0;                  // m/s
    uint32_t seed = 1;
};

// The frame's height fog (Passes/Atmosphere/FogVolume.hlsli) - weather content like the cloud layer. enabled false: the
// quality file's atmosphere.fog decides (its own enabled switch and medium); true: this medium, whatever the file says.
// The volume's grid (cell size, slices, distances) stays the quality file's.
struct FogDesc
{
    bool enabled = false;
    float density = 0.002f;        // extinction (1/m) at 'height'
    float heightFalloff = 0.02f;   // the density halves every 1 / this metres of height
    float height = 0;              // world metres
    float albedo[3] = { 1, 1, 1 }; // scattering / extinction
    float phaseG = 0.2f;           // Henyey-Greenstein asymmetry, (-1, 1)
    float startDistance = 0;       // m: no fog nearer than this
    float skyAmount = 1;           // [0, 1]: how much of the fog sky pixels take
    float noiseAmount = 0.3f;      // [0, 1]: the density's variation about its mean (0: a uniform medium)
    float noiseScale = 20;         // m: the variation's largest features
};

// A local fog volume: extra extinction inside an ellipsoid or a box, added to the frame's height fog in the fog's cells
// (FogVolume.hlsli; lit as the cells are: the sun and its shadows, the local lights, the indirect light). Only inside the
// fog's near volume (atmosphere.fog.volumetric_distance_m): a volume farther than that is not seen. The fog's volume runs
// when the frame has any, also without height fog (atmosphere.fog.local_volumes).
struct FogVolumeDesc
{
    double centre[3] = { 0, 0, 0 };  // world, m
    float halfSize[3] = { 1, 1, 1 }; // m: the ellipsoid's radii or the box's half extents, along the volume's axes
    float yaw = 0;                   // rad: the volume's turn about the world's up axis
    uint32_t shape = 0;              // 0 ellipsoid, 1 box
    float density = 0.05f;           // extinction (1/m) at the volume's bottom, away from its boundary
    float heightFalloff = 0;         // the density halves this many times from the volume's bottom to its top (0: uniform)
    float edge = 0.3f;               // (0, 1]: the outer share of the volume over which the density fades to 0 at the boundary
    float albedo[3] = { 1, 1, 1 };   // scattering / extinction
    // Rising steam (a bath, a kettle, a vent): the volume's own density variation and its source. All 0: the volume as
    // it was (the fog's own slow variation alone).
    float sourcePlane = 0;           // [0, 0.95]: the height inside the volume (0 bottom, 1 top) the medium rises from - no
                                     // density under it, heightFalloff counts from it (the water's surface in a volume that
                                     // reaches under it)
    float riseSpeed = 0;             // m/s: the variation's pattern moves up the volume's axis at this speed
    float turbulence = 0;            // [0, 1]: the variation's share of the density (0.5: from nothing to twice the mean;
                                     // 1: wisps with gaps); a quarter of it at the source plane, all of it from a third of
                                     // the height up - a sheet over the water that breaks up as it rises
    float turbulenceScale = 0.5f;    // m: the variation's largest features (three octaves down to a quarter of it), curled
                                     // sideways by a slower one
    // A density grid of the game's own (a simulation, authored wisps): R8 texels, x fastest then y then z, over the
    // volume's box [-1, 1]^3 along its axes - texel (0, 0, 0) at the corner (-1, -1, -1), the last at (1, 1, 1), read
    // between texels; a texel is the density's factor (255: 1). It multiplies everything above. Each side 1 .. 32
    // (kFogGridMax). The pointer stays valid until the frame is recorded (as WindFrame::records). null: none.
    const uint8_t* grid = nullptr;
    uint32_t gridSize[3] = { 0, 0, 0 };
};
constexpr uint32_t kMaxFogVolumes = 16;  // (the first ones of a frame take effect)
constexpr uint32_t kFogGridMax = 32;     // texels per side of a volume's density grid

// The frame's colour grading, scene-referred, before the tone curve (Passes/Shading/PostGradeLut.hlsl, Post.cpp; the
// reference's post process colour grading) - a look the game sets per frame, like the fog. enabled false: the quality
// file's shading.post_grading_* decide; true: these values, whatever the file says. Neutral values (the defaults) leave
// the picture as it is. Each of a range's five values is r, g, b and a master that multiplies (offset: adds to) them;
// the shadows', midtones' and highlights' values combine with the global ones (products; offsets: sums).
struct ColorGradingRange
{
    float saturation[4] = { 1, 1, 1, 1 };  // about the luma (0: grey)
    float contrast[4] = { 1, 1, 1, 1 };    // about scene grey 0.18, an exponent
    float gamma[4] = { 1, 1, 1, 1 };       // the value to the power 1 / gamma
    float gain[4] = { 1, 1, 1, 1 };
    float offset[4] = { 0, 0, 0, 0 };
};
struct ColorGradingDesc
{
    bool enabled = false;
    float temperature = 6500;      // K: the scene's white the picture is balanced from (6500: none)
    float tint = 0;                // across the temperature's line (+ green, - magenta; 1 = 0.05 in CIE 1960 uv)
    ColorGradingRange global, shadows, midtones, highlights;
    float shadowsMax = 0.09f;      // the luma (ACEScg) below which the shadows' values weigh in
    float highlightsMin = 0.5f;    // ... from which the highlights' values weigh in, fully from highlightsMax
    float highlightsMax = 1.0f;
};
// The scene description's weather (scene::Scene::clouds, fog, fogVolumes): FrameRenderer gives a frame the scene's cloud
// layer, fog or fog volumes where the frame brings none of its own (coverage 0, enabled false, no volumes) while that
// item's bit is set in FrameContext::sceneWeather. A producer that decides an item itself clears its bit, and its "none"
// is then none (a gate's --clouds 0 on a scene with clouds).
constexpr uint32_t kSceneClouds = 1, kSceneFog = 2, kSceneFogVolumes = 4;

// A14 (FEATURES_GAME 8; Requests/20260926_C_per_view_history.md): a full auxiliary view drawn in this frame before the
// main view (render-texture camera, mirror, portal, split screen). Its id is stable across frames (the key of every
// track's per-view history; nonzero, unique). 'reads' lists the views whose outputs this view's materials read: those
// are drawn first; an edge on a cycle reads that view's previous-frame output instead.
struct AuxView
{
    uint32_t id = 0;
    ViewDesc view;                // kind RenderTexture, Mirror, Portal or Split
    TextureRef output;            // RenderTexture / Mirror / Portal: RGBA16F linear radiance (exposed); Split: the display
                                  // target (per-view post: render A)
    std::vector<uint32_t> reads;
};

struct FrameContext
{
    uint64_t frameIndex = 0;
    double time = 0;
    float deltaTime = 0;
    ViewDesc mainView;
    CloudLayerDesc clouds;  // B5 (v1.77): coverage 0 = none
    FogDesc fog;            // the height fog (enabled false: the quality file's)
    std::vector<FogVolumeDesc> fogVolumes;  // local fog volumes (at most kMaxFogVolumes take effect)
    ColorGradingDesc grading;  // the colour grading before the tone curve (enabled false: the quality file's)
    uint32_t sceneWeather = kSceneClouds | kSceneFog | kSceneFogVolumes;  // the scene's weather fills the items above that are empty
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
    // v1.91 (A, defect queue 6 / game request 83): the camera's white balance - the scene illuminant the camera is set to,
    // as a correlated colour temperature (K; 0 = D65: no adaptation) and a tint (Duv in CIE 1960 uv: + green, - magenta).
    // M's post chain adapts the exposed image from it to the display's D65 (Bradford, Post.cpp) when
    // shading.post_white_balance is on; otherwise the frame is bit-identical to the D65 output.
    float whiteBalanceKelvin = 0, whiteBalanceTint = 0;
    // v1.50 (A15, E): GPU pass timings of the last completed frame (GpuProfiler::lastCompleted), shown by the debug HUD
    // (quality key debug.hud); null = no timings (the HUD says so). The host keeps it valid until record() returns.
    const FrameTiming* timing = nullptr;
    CelestialFrame celestial;  // v1.49 (B4): moon, stars, airglow (S publishes FrameResources::celestial)
    WindFrame wind;            // v1.50 (B6): the World's wind records of this tick (S publishes FrameResources::wind)
    WeatherFrame weather;      // v1.50 (B6): rain, wetness, snow, fog, cloud cover
    // Origin rebase (C9, request 20260926_C_origin_rebase.md): this frame's world coordinates are the previous frame's
    // minus originShift (whole multiples of 1024 m per axis). The host calls GpuScene::rebase(originShift) before it
    // applies the frame's transforms; FrameRenderer moves the previous view; tracks move their world-space state (V's
    // previous camera position and instance chunks, S's clipmap pages, R's GI cells and TLAS) by the same amount.
    float3 originShift{};
    // The frame's coordinates in the renderer's world: world = frame + worldOrigin (the host's accumulated origin shifts;
    // 0 without any). Consumers of world-anchored data that does not rebase (the particle stream's anchors) use it.
    double worldOrigin[3] = { 0, 0, 0 };
    // Particle stream space (the VFX World's axes, NativeVfxStream.h) -> the renderer's world, axis signs: the Unity host's
    // World is the renderer's world mirrored in z ({1, 1, -1}, set once at the host boundary: UnxRendererCreate); tools and
    // tests stream in renderer axes ({1, 1, 1}). The particle module simulates in stream space and maps at its outputs.
    float streamAxes[3] = { 1, 1, 1 };
    // v1.70 (B8, engine 1 W): this frame's GPU fluids (valid until record() returns; none: fluidCount 0).
    const FluidFrame* fluids = nullptr;
    uint32_t fluidCount = 0;
    // v1.72 (B7, engine 1 W): this frame's sea (null: none; valid until record() returns). Time: 'time', camera: mainView.
    const OceanFrame* ocean = nullptr;
    // v1.78 (W2, engine 2 W): this frame's closed basins (valid until record() returns; none: poolCount 0). Time: 'time'.
    const PoolFrame* pools = nullptr;
    uint32_t poolCount = 0;
    // A14: this frame's auxiliary views (at most FrameRenderer::kMaxViewsPerFrame - 1 with the main view).
    std::vector<AuxView> auxViews;
    // Temporal upscale (output.render_height_max; set by FrameRenderer::record, not by hosts): outputWidth != 0 means the
    // main view renders at mainView.width x height (internal) with a sub-pixel jitter of its projection and M's temporal
    // upscale (Upscale.cpp) reconstructs the output resolution before the post chain. viewProj / prevViewProj are the
    // unjittered matrices of this and the previous frame (mainView carries the jittered ones: every history of the
    // frame reprojects onto the previous frame's jittered samples), proj the unjittered projection; jitter in internal
    // pixels (+x right, +y down: the image content moves by it), prevJitter the previous frame's (= jitter after a
    // reset); exposureRatio = this frame's exposure over the previous one's; reset: no history.
    struct Upscale
    {
        uint32_t outputWidth = 0, outputHeight = 0;
        float jitterX = 0, jitterY = 0, prevJitterX = 0, prevJitterY = 0;
        float4x4 viewProj{}, prevViewProj{}, proj{};
        float exposureRatio = 1;
        bool reset = true;
    } upscale;
};
constexpr float kOriginGrid = 1024.0f;
constexpr uint32_t kGpuSimulationSoft = 1, kGpuSimulationVfx = 2, kGpuSimulationRigid = 4;
constexpr uint32_t kDiscontinuityRestore = 1, kDiscontinuityCut = 2;
} // namespace unx::render
