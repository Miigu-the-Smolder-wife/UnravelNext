#pragma once
// Water surface height clipmap (FEATURES_GAME 1.8; kernels OceanHeight.hlsli, OceanRefine.hlsli): each frame the ocean's
// displaced surface is solved once as a world-space height field on L camera-centred levels of 512^2 points, with
// max/min mips, so the water layer finds each pixel's exact first intersection (oceanRefine).
#include "unx/water/Ocean.h"

#include <cstdint>

namespace unx::water
{
struct OceanHeightDesc
{
    uint32_t levels = 12;       // L: s_0 2^(L-1) x 512 / 2 must reach the farthest water seen
    float s0 = 0.015625f;       // finest spacing (m): half the finest cascade texel (FEATURES_GAME 1.8)
    float waterLevel = 0.0f;    // the still surface's height (m)
    float foldRadius = 4.0f;    // R (m): bound of the horizontal displacement (the fold search's reach)
};
struct OceanHeightView
{
    float camera[3] = {};       // world position (m)
    float pixelAngle = 0.0f;    // radians per pixel at the image centre
    float farDistance = 5000.0f;
};
struct OceanHeightOutput
{
    render::TextureRef height;   // Texture2DArray RGBA32F, L slices: H, x0 - w (2), flags
    render::TextureRef bounds;   // Texture2DArray RG32F, L slices, 9 mips: max, min
    render::BufferRef params;    // raw parameter buffer for oceanRefine (OceanRefine.hlsli)
    render::BufferRef folds;     // uint 2: points flagged unsolved (3), points past a scatter triangle's loop bound
};

class OceanHeight
{
public:
    static constexpr uint32_t kN = 512, kMips = 9;
    OceanHeight(render::Device& device, render::ShaderLibrary& shaders, const OceanHeightDesc& desc);
    ~OceanHeight();
    OceanHeight(const OceanHeight&) = delete;
    OceanHeight& operator=(const OceanHeight&) = delete;
    OceanHeightOutput record(render::RenderGraph& graph, const OceanOutput& fields, const float cascadeLengths[3], const OceanHeightView& view);
    const OceanHeightDesc& desc() const { return m_desc; }

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    OceanHeightDesc m_desc;
    render::ComPtr<ID3D12Resource> m_height, m_bounds, m_params, m_folds, m_paramUpload, m_keys;
    uint32_t m_boundsUav[kMips] = {}, m_heightSrv = 0, m_boundsSrv = 0, m_slot = 0;
};
} // namespace unx::water
