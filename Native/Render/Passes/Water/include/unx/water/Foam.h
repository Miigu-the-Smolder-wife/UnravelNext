#pragma once
// Water foam F (FEATURES_GAME 1.3 (f)-F; kernels Foam.hlsli): the decaying maximum of the breaking fraction on a
// world-space clipmap around the camera; the water shading reads it at a pixel's rest position (foamAt).
#include "unx/water/Ocean.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace unx::water
{
struct FoamDesc
{
    uint32_t levels = 5;      // at most 5 (Foam.hlsli FOAM_LEVELS)
    float s0 = 0.03125f;      // finest texel (m): the finest cascade's texel
    float tau = 4.0f;         // foam lifetime (s)
    float threshold = 0.3f;   // J_t: breaking where the Jacobian is below it
};
struct FoamOutput
{
    render::TextureRef foam;  // Texture2DArray R16F, 1024^2, one slice per level (toroidal)
    uint32_t paramSrv = 0;    // raw parameter buffer (Foam.hlsli layout), valid for this frame's passes
    render::BufferRef variance; // raw uint64 per level: sigma_J^2 in fixed point 2^-40 (FoamVariance.hlsl)
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
    FoamOutput record(render::RenderGraph& graph, uint64_t frame, double seconds, const OceanOutput& fields, const float cascadeLengths[3], double cameraX,
                      double cameraZ);
    void setDesc(const FoamDesc& desc);
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
    double m_updated[kMaxLevels] = {};
    bool m_placed[kMaxLevels] = {};
    bool m_varianceValid = false;
};
} // namespace unx::water
