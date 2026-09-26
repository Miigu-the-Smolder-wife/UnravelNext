#pragma once
// Water foam F (FEATURES_GAME 1.3 (f)-F; kernels Foam.hlsli): the decaying maximum of the breaking fraction on a
// world-space clipmap around the camera; the water shading reads it at a pixel's rest position (foamAt).
#include "unx/water/Ocean.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace unx::water
{
struct FoamDesc
{
    uint32_t levels = 5;      // at most 5 (Foam.hlsli FOAM_LEVELS)
    float s0 = 0.03125f;      // finest texel (m): the finest cascade's texel
    float tau = 4.0f;         // foam lifetime (s)
    // J_t (breaking where the Jacobian is below it): NaN = automatic per sea state, so the foam coverage equals the
    // observed whitecap coverage `coverage` (FoamCalibrate.hlsl); a number overrides it (a game's look).
    float threshold = std::numeric_limits<float>::quiet_NaN();
    // The target foam coverage for the automatic threshold: NaN = Monahan & O'Muircheartaigh (1980) W = 3.84e-6 U^3.41
    // for the sea's 10 m wind U (m/s).
    float coverage = std::numeric_limits<float>::quiet_NaN();
};
struct FoamOutput
{
    render::TextureRef foam;  // Texture2DArray R16F, 1024^2, one slice per level (toroidal)
    uint32_t paramSrv = 0;    // raw parameter buffer (Foam.hlsli layout), valid for this frame's passes
    render::BufferRef variance; // raw: per-level sigma_J^2, gradient covariances, J_t (FoamVariance.hlsl layout)
};

class Foam
{
public:
    static constexpr uint32_t kN = 1024, kMaxLevels = 5;
    Foam(render::Device& device, render::ShaderLibrary& shaders, const FoamDesc& desc, uint32_t framesInFlight = 2);
    ~Foam();
    Foam(const Foam&) = delete;
    Foam& operator=(const Foam&) = delete;
    // Level l updates when frame % 2^l == 0 or its window moved (the elapsed time since its last update decays it); the
    // variances are recomputed on a new sea state (fields.previousValid false) and on the first record.
    FoamOutput record(render::RenderGraph& graph, uint64_t frame, double seconds, const OceanOutput& fields, const OceanDesc& sea, double cameraX, double cameraZ);
    // The automatic threshold's target coverage for a sea (Monahan & O'Muircheartaigh 1980, capped at 1).
    static float observedCoverage(float windSpeed) { return std::min(1.0f, 3.84e-6f * std::pow(std::max(windSpeed, 0.0f), 3.41f)); }
    void setDesc(const FoamDesc& desc);
    // Origin rebase (FrameContext::originShift): the world coordinates become the previous ones minus the shift (m, a
    // whole number of every level's texels, e.g. multiples of 1024 m). Every texel keeps its foam and storage.
    void rebase(double shiftX, double shiftZ);
    const FoamDesc& desc() const { return m_desc; }
    static int32_t origin(double camera, float spacing) { return int32_t(std::floor(camera / double(spacing))) - int32_t(kN / 2); }

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    FoamDesc m_desc;
    render::ComPtr<ID3D12Resource> m_foam, m_variance;
    std::vector<render::ComPtr<ID3D12Resource>> m_upload;
    std::vector<uint8_t*> m_mapped;
    std::vector<uint32_t> m_paramSrv;
    int32_t m_origin[kMaxLevels][2] = {};
    int32_t m_bias[kMaxLevels][2] = {};  // storage = (world texel + bias) mod kN
    double m_updated[kMaxLevels] = {};
    bool m_placed[kMaxLevels] = {};
    bool m_varianceValid = false;
    float m_calibratedWind = -1;  // the wind the automatic threshold was solved for
};
} // namespace unx::water
