#pragma once
// Water view grid (FEATURES_GAME 1.8 B; kernels ViewGrid.hlsli): the camera's water surface at the density its visible
// detail needs. The far field is a grid of world azimuth (one pixel across) x rest-distance rows (spacing min(one pixel
// row on the still plane, 2.5 pixel footprints)), displaced and scattered into the pixels (watertight compute raster,
// 64-bit atomic min of view depth | triangle id); each pixel's winning triangle is polished onto the continuous surface
// along the pixel's ray. The near field (rest distance < nearRadius) is a separate grid (FEATURES_GAME 1.8 B.2).
#include "unx/water/Ocean.h"

#include <cstdint>
#include <vector>

namespace unx::water
{
struct ViewGridCamera
{
    float position[3] = {};
    float forward[3] = { 1, 0, 0 }, right[3] = { 0, 0, 1 }, up[3] = { 0, 1, 0 };  // orthonormal, world
    float tanX = 0.57735f, tanY = 0.32476f;  // tan of the half field of view
    uint32_t width = 0, height = 0;
};
struct ViewGridWater
{
    float level = 0;          // still water height (m)
    float extent = 1.0e6f;    // the water body's extent from the camera (m, horizontal; open sea: the horizon)
    float nearRadius = 16;    // the far field starts here (m, horizontal)
    float bound = 3.5f;       // horizontal + vertical displacement bound (m)
    bool lake = false;        // a circular water body (tests) instead of the open sea
    float lakeCentre[2] = {}, lakeRadius = 0;
};
struct ViewGridLayout
{
    std::vector<float> params;  // the parameter buffer (ViewGrid.hlsli): 32 floats of header, then the row table
    uint32_t columns = 0, rows = 0;
    float window[4] = {};       // the screen's angular window: azimuth min, max, elevation min, max (rad)
};
struct ViewGridOutput
{
    render::TextureRef surface;  // RGBA32F: rest position x0 (x, z), view depth, flags (0 none, 1 polished, 2 mesh)
    render::TextureRef depth;    // R32F view depth, +inf where no water
    render::BufferRef keys;      // raw, 8 B per pixel: triangle id (low), depth bits (high); ~0 = none
    render::BufferRef counters;  // uint: triangles past the scatter's 8 x 8 loop
    uint32_t columns = 0, rows = 0;
};

class ViewGrid
{
public:
    static constexpr uint32_t kMaxRows = 32768;
    ViewGrid(render::Device& device, render::ShaderLibrary& shaders, uint32_t framesInFlight = 2);
    ~ViewGrid();
    ViewGrid(const ViewGrid&) = delete;
    ViewGrid& operator=(const ViewGrid&) = delete;
    // The far-field grid of a camera: its angular window widened by the displacement bound, the rows by rest distance.
    static ViewGridLayout layout(const ViewGridCamera& camera, const ViewGridWater& water, const float cascadeLengths[3]);
    ViewGridOutput record(render::RenderGraph& graph, uint64_t frame, const OceanOutput& fields, const float cascadeLengths[3], const ViewGridCamera& camera,
                          const ViewGridWater& water);

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    std::vector<render::ComPtr<ID3D12Resource>> m_upload;
    std::vector<uint8_t*> m_mapped;
    std::vector<uint32_t> m_srv;
};
} // namespace unx::water
