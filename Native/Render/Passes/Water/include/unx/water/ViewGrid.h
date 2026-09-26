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
    float nearPlane = 0.05f;  // view depth (m) of the renderer's near plane (ViewDesc::nearPlane)
};
struct ViewGridWater
{
    float level = 0;          // still water height (m)
    float extent = 1.0e6f;    // the water body's extent from the camera (m, horizontal; open sea: the horizon)
    float nearRadius = 16;    // the far field starts here (m, horizontal)
    // R and A until the ocean's own bounds are measured (the bounds pyramid's top mip, read back framesInFlight + 1 frames
    // later, times 1.25 for the sea's change over those frames); then the measured ones. After a new sea state or spectrum
    // (OceanOutput::previousValid false) the larger of these and the last measurement, until the new state is measured:
    // configure the bound of the game's roughest sea.
    float horizontalBound = 1.5f;  // R: bound of the horizontal displacement (m)
    float verticalBound = 2.0f;    // A: bound of the displacement's height above and below the still water (m)
    float bound() const { return horizontalBound + verticalBound; }
    bool lake = false;        // a circular water body (tests) instead of the open sea
    float lakeCentre[2] = {}, lakeRadius = 0;
};
struct ViewGridLayout
{
    std::vector<float> params;  // the parameter buffer (ViewGrid.hlsli): 32 floats of header, then the row table
    uint32_t columns = 0, rows = 0;
    float window[4] = {};       // the screen's angular window: azimuth min, max, elevation min, max (rad)
    uint32_t nearLevels = 0;       // adaptive near-field levels
    int32_t nearFirst[2] = {};      // the top level's first block (level-local block coordinates)
    uint32_t nearWidth = 0;        // the top level's blocks per side
};
struct ViewGridOutput
{
    render::TextureRef surface;  // RGBA32F: rest position x0 (x, z), view depth, flags (0 none, 1 polished, 2 mesh)
    render::TextureRef depth;    // R32F view depth, +inf where no water
    render::BufferRef keys;      // raw, 8 B per pixel: triangle id (low), depth bits (high); ~0 = none
    render::BufferRef counters;  // uint: big triangles (rasterised by tiles), big triangles lost past the list (0), big tiles
    render::TextureRef error;    // diagnostics only: R32F normal distance to the surface / pixel footprint of each output point
    render::BufferRef nearDrawn; // diagnostics only: the adaptive near field's drawn blocks (raw: count, then (level, x, z) x 16 B)
    uint32_t columns = 0, rows = 0;
    ViewGridLayout layout;       // the layout this record used
    ViewGridWater water;         // its water description (the measured bounds once available)
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
                          const ViewGridWater& water, bool diagnostics = false);

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    std::vector<render::ComPtr<ID3D12Resource>> m_upload;
    std::vector<uint8_t*> m_mapped;
    std::vector<uint32_t> m_srv;
    render::ComPtr<ID3D12CommandSignature> m_dispatch;
    render::ComPtr<ID3D12Resource> m_pyramid;  // ocean bounds pyramid (OceanBounds.hlsl)
    uint32_t m_pyramidUav[10] = {};
    render::ComPtr<ID3D12Resource> m_drawn;  // diagnostics' drawn-block list
    uint32_t m_drawnUav = 0;
    std::vector<render::ComPtr<ID3D12Resource>> m_boundsReadback;  // the pyramid's top mip per frame slot
    std::vector<uint64_t> m_boundsFrame;                           // the frame whose bounds a slot holds (+1; 0 = none)
    bool m_measured = false;
    uint64_t m_seaChanged = 0;                                     // frame + 1 of the last new sea state
    uint64_t m_measuredFrame = 0;                                  // frame + 1 of the measurement in use
    float m_measuredBounds[2] = {};                                // R, A
};
} // namespace unx::water
