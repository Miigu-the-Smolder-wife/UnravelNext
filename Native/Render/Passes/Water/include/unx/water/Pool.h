#pragma once
// Closed basins W2 (bath, swimming pool; FEATURES_GAME 1.10; kernels Pool.hlsli): the linear water surface of a
// rectangular basin with reflecting walls, evolved exactly in the spectral domain (the basin's cosine modes: the even
// mirror of its 257 x 257 samples is a 512^2 periodic field; the state is the 257^2 mode amplitudes, rotated exactly
// each frame with Ripple's gravity-capillary dispersion and damped by the bulk and the boundary layers), sources from
// bodies and contacts (impulse, displaced volume: the water's volume is conserved), and its surface
// as a layer-1 triangle stream (256^2 cells, exact spectral normals, exact previous-frame motion).
#include "unx/render/Device.h"
#include "unx/render/FrameResources.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstdint>
#include <vector>

namespace unx::render
{
struct FramePassContext;
class TrackState;
}

namespace unx::water
{
// The surface's statistics of one record (PoolStats.hlsl; FEATURES_GAME 1.10 "max |eta - mean|"; the host's
// UnxPoolStatsLatest): over the 257^2 samples, relative to the still level. Read back framesInFlight records after the
// record they describe (the caller waited for that frame).
struct PoolStats
{
    bool valid = false;        // false until a record's statistics completed on the GPU
    uint64_t frame = 0;        // the record's frame index
    double time = 0;           // the basin's time at that record (s)
    float mean = 0;            // mean eta (m)
    float rms = 0;             // RMS of eta - mean (m)
    float maxDeviation = 0;    // max |eta - mean| (m)
};

// W's waterGeometry step for FrameContext::pools (PoolTrack.cpp): each basin in view or with sources this frame becomes a
// layer-1 triangle stream.
void poolGeometry(render::FramePassContext& fc);
// The latest statistics of every basin in the frame's set (id, PoolStats; valid ones only), from the track state
// (render thread: the host copies them after each record and serves UnxPoolStatsLatest from its copy).
void poolStatsSnapshot(render::TrackState& state, std::vector<std::pair<uint32_t, PoolStats>>& out);

struct PoolDesc
{
    float sizeX = 4.0f, sizeZ = 3.0f;       // inner basin Lx, Lz (m): walls at local 0 and L on each axis
    float depth = 1.0f;                     // uniform water depth d (m); 0 = deep water
    float gravity = 9.81f;
    float tensionOverDensity = 7.28e-5f;    // sigma / rho (m^3/s^2): clean water at 20 C
    float viscosity = 1.0e-6f;              // kinematic nu (m^2/s): the bulk and the floor's and walls' boundary layers
    float surfaceFilm = 0.0f;               // 0: a clean surface; 1: an inextensible film (bathers, soap): the surface's own
                                            // boundary layer damps short ripples about 10 x faster (exact at 0 and 1)
    uint32_t maxSources = 1024;             // per frame
    uint32_t framesInFlight = 2;            // upload slots
};
// Where the basin is: the still surface's centre (world, this frame's coordinates: y = still level, the level of the
// water with no bodies in it) and its yaw about +y (rad): local x -> (cos yaw, 0, -sin yaw), local z -> (sin yaw, 0, cos yaw).
struct PoolPlacement
{
    double centre[3] = {};
    float yaw = 0;
};
struct PoolSource
{
    double x = 0, z = 0;     // world position of the centre (m); inside the basin
    float radius = 0.05f;    // Gaussian footprint sigma (m); at least the larger sample spacing
    float impulse = 0;       // vertical impulse on the water (N s, positive = pushed down)
    float volume = 0;        // change of the displaced volume at the footprint (m^3, positive = water pushed out: a body
                             // entering or moving there; negative where it left): the surface under it goes down by V,
                             // the basin's mean level is unchanged (volume conserved)
};
struct PoolOutput
{
    render::TextureRef field;  // RGBA32F 257^2: (eta, d eta / dx, d eta / dz, phi) at sample (i, j) = local (i hx, j hz)
    render::TriangleStream stream;  // layer 1, material 0 (the caller sets it)
};

class Pool
{
public:
    static constexpr uint32_t kN = 512, kQ = 257, kCells = 256;
    Pool(render::Device& device, render::ShaderLibrary& shaders, const PoolDesc& desc);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Advances the basin to `time` (s, the host's clock for it; the first call starts it there) and writes its surface.
    // frameDt: the time since the previous frame (the stream's motion). Evolution between calls is exact whatever the
    // interval: a basin that was not recorded for a while (out of view, no sources) is advanced in two steps, to
    // time - frameDt and then to time, so its motion is still the last frame interval's. This frame's sources act at
    // time - frameDt; a source whose centre lies outside the basin fails.
    PoolOutput record(render::RenderGraph& graph, uint64_t frame, const PoolPlacement& placement, double time, float frameDt,
                      const std::vector<PoolSource>& sources);
    // Replaces the state (eta, phi per sample, row-major z then x, 2 x 257^2 floats); tests and restores.
    void setState(const std::vector<float>& etaPhi);
    // World (x, z) -> basin sample coordinates (0..256 inside, per axis).
    static void toSamples(const PoolDesc& desc, const PoolPlacement& placement, double x, double z, float& u, float& v);
    static bool contains(const PoolDesc& desc, const PoolPlacement& placement, double x, double z);
    const PoolDesc& desc() const { return m_desc; }
    bool started() const { return m_started; }
    // Conservative visibility of the basin's bounds (still level +- vertical) in a view: false when all eight corners lie
    // outside one clip plane (x, y against w, or behind the camera).
    static bool visible(const PoolDesc& desc, const PoolPlacement& placement, const float4x4& viewProj);
    double time() const { return m_time; }
    // The statistics of the record framesInFlight records before the latest (valid once that many records completed).
    const PoolStats& latestStats() const { return m_latestStats; }

private:
    struct Refs
    {
        render::BufferRef modes, input, accum, previous, twiddles, table;
        render::TextureRef field;
    };
    void evolve(render::RenderGraph& g, const Refs& refs, float dt, uint32_t sourceSrv, uint32_t sourceCount, float meanShift, bool replace);

    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    PoolDesc m_desc;
    uint64_t m_topologyId = 0;
    render::ComPtr<ID3D12Resource> m_modes, m_input, m_accum, m_previous, m_twiddles, m_table, m_tableUpload, m_output, m_stateUpload;
    render::ComPtr<ID3D12Resource> m_indices;
    bool m_indicesReady = false;
    render::ComPtr<ID3D12Resource> m_stats;                            // PoolStats.hlsl: 257 row partials + the result
    std::vector<render::ComPtr<ID3D12Resource>> m_statsReadback;       // framesInFlight + 1 slots of the result (16 B)
    std::vector<uint64_t> m_statsFrame;
    std::vector<double> m_statsTime;
    uint64_t m_records = 0;
    PoolStats m_latestStats;
    std::vector<render::ComPtr<ID3D12Resource>> m_sourceUpload;
    std::vector<uint8_t*> m_sourceMapped;
    std::vector<uint32_t> m_sourceSrv;
    double m_time = 0;
    bool m_started = false, m_initialised = false, m_stateDirty = false;
    bool m_pristineEquilibrium = true; // exact initial zero field, before any state replacement or nonzero source
};
} // namespace unx::water
