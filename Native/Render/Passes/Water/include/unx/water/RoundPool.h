#pragma once
// Round basins W2-R (defect queue 13 (74); FEATURES_GAME 1.10 (c); RoundPool.hlsli): the linear water surface of a
// circular basin of radius R with a reflecting wall, evolved exactly in the basin's own modes J_m(k_mn r) e^{i m theta}
// with J_m'(k_mn R) = 0 (no flux through the wall: a wave reflects with no loss and no phase error), Ripple's
// gravity-capillary dispersion over the depth and the boundary-layer damping of the floor, the cylindrical wall and an
// inextensible film, all in double on the host. Samples: N_theta = 512 angles x N_r = 128 rings (r_j = (j + 1) R / N_r,
// the last on the wall) plus the centre. The transform is the angular FFT per ring and, per order m, the least-squares
// projection onto the radial modes (host double: F_m = (B^T W B)^-1 B^T W, weights r_j; synthesis B_m; the radial
// slope D_m = k J_m'(k r)); modes are truncated at the radial Nyquist k R <= pi N_r. The state is the modes' complex
// amplitudes (H, Phi) for m = 0 .. 255 (negative orders by conjugate symmetry: the field is real).
#include "unx/render/Device.h"
#include "unx/render/FrameResources.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstdint>
#include <vector>

namespace unx::water
{
struct RoundPoolDesc
{
    float radius = 1.0f;                    // inner radius R (m)
    float depth = 0.6f;                     // uniform water depth d (m); 0 = deep water
    float gravity = 9.81f;
    float tensionOverDensity = 7.28e-5f;    // sigma / rho (m^3/s^2)
    float viscosity = 1.0e-6f;              // kinematic nu (m^2/s)
    float surfaceFilm = 0.0f;               // 0 clean, 1 inextensible film
    uint32_t maxSources = 1024;             // per frame
    uint32_t framesInFlight = 2;
};
struct RoundPoolPlacement
{
    double centre[3] = {};                  // the still surface's centre (world, this frame's coordinates)
    float yaw = 0;                          // about +y (rad): local x -> (cos, 0, -sin), local z -> (sin, 0, cos)
};
struct RoundPoolSource
{
    double x = 0, z = 0;                    // world position (m), inside the basin
    float radius = 0.05f;                   // Gaussian sigma (m)
    float impulse = 0;                      // N s, + downward
    float volume = 0;                       // m^3, + water pushed out of the footprint
};
struct RoundPoolOutput
{
    render::TextureRef field;               // RGBA32F N_theta x N_r: (eta, d eta / dr, (1 / r) d eta / d theta, phi) at (theta_i, r_j)
    render::BufferRef centre;               // raw float4: (eta, phi, previous eta, 0) at r = 0
    render::TriangleStream stream;          // layer 1 (the caller sets the material)
};

// Host-side mode tables (double), also the unit tests' API.
double roundBesselJ(int m, double x);                        // J_m(x), m >= 0
double roundBesselJPrime(int m, double x);                   // J_m'(x)
std::vector<double> roundDiniRoots(int m, double xMax, uint32_t maxCount);  // roots x of J_m'(x) in (0, xMax] (m = 0: 0 first)
struct RoundTables
{
    static constexpr uint32_t kTheta = 512, kRings = 128, kOrders = 256;
    std::vector<uint32_t> modeStart, modeCount;  // modeStart: per order m the first mode index [0, 256), then the analysis
                                                 // offsets [256, 512), then the synthesis / slope offsets [512, 768); modeCount per order
    std::vector<float> modes;       // per mode: w, K / w, w / K, delta (PoolEvolve's table)
    std::vector<double> k;          // per mode: k = x / R
    std::vector<float> analysis;    // per order: F_m, count x rings (row n: the weights over rings)
    std::vector<float> synthesis;   // per order: B_m, rings x count (row j: the modes at ring j)
    std::vector<float> slope;       // per order: D_m = k J_m'(k r_j), rings x count
    uint32_t modeTotal = 0;
    double radius = 0;
};
RoundTables roundTables(const RoundPoolDesc& desc);

class RoundPool
{
public:
    static constexpr uint32_t kTheta = RoundTables::kTheta, kRings = RoundTables::kRings, kOrders = RoundTables::kOrders;
    static constexpr uint32_t kTriangles = kTheta * (1 + 2 * (kRings - 1));  // the centre fan and the ring quads
    RoundPool(render::Device& device, render::ShaderLibrary& shaders, const RoundPoolDesc& desc);
    ~RoundPool();
    RoundPool(const RoundPool&) = delete;
    RoundPool& operator=(const RoundPool&) = delete;
    // As Pool::record: advances the basin to 'time' (exact over any interval; two steps after a gap) and writes its
    // surface; this frame's sources act at time - frameDt. A source outside the basin fails.
    RoundPoolOutput record(render::RenderGraph& graph, uint64_t frame, const RoundPoolPlacement& placement, double time, float frameDt,
                           const std::vector<RoundPoolSource>& sources);
    static bool contains(const RoundPoolDesc& desc, const RoundPoolPlacement& placement, double x, double z);
    const RoundPoolDesc& desc() const { return m_desc; }
    const RoundTables& tables() const { return m_tables; }
    bool started() const { return m_started; }
    double time() const { return m_time; }

private:
    struct Refs
    {
        render::BufferRef modes, increments, accum, spectrum, previous, twiddles, table, analysis, synthesis, slope, orders, centre;
        render::TextureRef field;
    };
    void evolve(render::RenderGraph& g, const Refs& r, float dt, uint32_t sourceSrv, uint32_t sourceCount, float meanShift);

    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    RoundPoolDesc m_desc;
    RoundTables m_tables;
    render::ComPtr<ID3D12Resource> m_modes, m_increments, m_accum, m_spectrum, m_previous, m_twiddles, m_table, m_analysis, m_synthesis, m_slope, m_orders,
        m_centre, m_output, m_tableUpload;
    std::vector<render::ComPtr<ID3D12Resource>> m_sourceUpload;
    std::vector<uint8_t*> m_sourceMapped;
    std::vector<uint32_t> m_sourceSrv;
    double m_time = 0;
    bool m_started = false, m_initialised = false;
};
} // namespace unx::water
