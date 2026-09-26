#pragma once
// Fluid surface reconstruction (track W, B8; FEATURES_GAME 0.B; kernels FluidSurface.hlsli). Particles of the physics
// fluid (engine 1 P10: positions in cells relative to the domain origin, 8 particles per cell) become the 0.5 level set
// of their quadratic B-spline density on a sparse node grid of spacing h = the particle spacing, extracted by marching
// cubes over the active 8^3-node blocks into a triangle list (world position, normal) with its draw arguments.
// Deterministic: fixed-point density sums, slots and triangles in block, cell and case-table order.
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"

#include <vector>
#include "unx/render/Shaders.h"

#include <array>
#include <cstdint>
#include <vector>

namespace unx::water
{
struct FluidSurfaceDesc
{
    uint32_t nodes[3] = {};      // node grid (multiples of 8): the fluid domain's cells x (particle units -> nodes)
    float scale = 2.0f;          // particle position units -> node units (fluid cells at 8 particles per cell: 2)
    float h = 0.025f;            // node spacing (m)
    float origin[3] = {};        // world position of node 0
    uint32_t maxParticles = 0;
    uint32_t maxBlocks = 0;      // active block pool (0 = every block of the grid)
    uint32_t maxTriangles = 0;
};
struct FluidSurfaceInput
{
    render::BufferRef particles;          // raw: position float3 at the start of each element
    render::BufferRef previous;           // optional: the previous tick's particles (blended by alpha), same order or indexed
    uint32_t count = 0, stride = 0;       // particles, bytes per particle
    float alpha = 1.0f;                   // frame time between the previous (0) and current (1) tick
    uint32_t velocityOffset = UINT32_MAX; // bytes to the particle velocity float3 (physics fluid: 16); UINT32_MAX = none
    float velocityScale = 0;              // particle velocity units -> m/s (physics fluid: dx, cells/s -> m/s)
    float axes[3] = { 1, 1, 1 };  // particle space -> output (renderer) axis signs (FrameContext::streamAxes: the host's
                                  // World is the renderer's mirrored in z); the grid, origin and bounds stay in particle space
    uint32_t previousSlotOffset = UINT32_MAX;  // bytes to the uint index of each particle in `previous` (physics fluid:
                                               // 44, Particle.origin); UINT32_MAX = the same index
    // W3 seam (engine 2): closed basins the fluid enters. Their water and the fluid are one medium, so the surface below
    // a basin's water (level + eta, W2 pool field) is cut away exactly (FluidSurface.hlsli fsAboveWater); only cells in
    // the waterline band change. Renderer axes, as the output.
    struct Basin
    {
        float centre[3] = {};        // the water's centre at its level (PoolPlacement::centre)
        float cosYaw = 1, sinYaw = 0;  // Pool.cpp axes(): local x = dx cos - dz sin, local z = dx sin + dz cos
        float sizeX = 0, sizeZ = 0;  // inner basin (m)
        render::TextureRef field;    // RGBA32F 257^2 pool field (eta in .x) of this frame
    };
    static constexpr uint32_t kMaxBasins = 64;  // FluidSurface.hlsli FS_CLIP_BASINS_MAX
    std::vector<Basin> basins;
};
struct FluidSurfaceOutput
{
    render::BufferRef vertices;  // 32 B per vertex: world position (w = 1), normal (w = 0); 3 per triangle, CCW outside;
                                 // triangles past the drawn count have NaN positions (inactive in a BLAS over the capacity)
    render::BufferRef velocities;  // 16 B per vertex: world velocity m/s (w = 0); zero without particle velocities
    render::BufferRef draw;      // D3D12_DRAW_ARGUMENTS
    render::BufferRef counters;  // uint: active blocks, triangles, overflow (pool or triangle capacity)
};

class FluidSurface
{
public:
    static constexpr uint32_t kVertexBytes = 32, kCaseStride = 25, kMaxCaseTriangles = 8;
    FluidSurface(render::Device& device, render::ShaderLibrary& shaders, const FluidSurfaceDesc& desc);
    ~FluidSurface();
    FluidSurface(const FluidSurface&) = delete;
    FluidSurface& operator=(const FluidSurface&) = delete;

    // Records the reconstruction into the graph (compute on the graphics queue). The outputs are the module's own
    // buffers, valid after the graph runs until the next record.
    FluidSurfaceOutput record(render::RenderGraph& graph, const FluidSurfaceInput& input);
    const FluidSurfaceDesc& desc() const { return m_desc; }
    // The node grid's world position for the next record (the fluid's origin in this frame's coordinates: origin rebase).
    void setOrigin(const float origin[3]) { for (int a = 0; a < 3; ++a) m_desc.origin[a] = origin[a]; }
    uint32_t blocks() const { return m_blocks[0] * m_blocks[1] * m_blocks[2]; }
    // World bounds of every possible vertex (the node grid, particle space): the stream's culling box (map with the axes).
    void bounds(float minimum[3], float maximum[3]) const;

    // Marching cubes case table: per case (bit c = corner c inside, corner bits x, y, z), the triangle count, then 3 edge
    // indices per triangle (edges FluidSurface.hlsli kFsEdges), CCW seen from outside (the lower density side). Faces
    // with four crossings separate the inside corners, the same choice on both cubes sharing the face: watertight.
    static const std::array<uint32_t, 256 * kCaseStride>& caseTable();
    static constexpr uint32_t kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    FluidSurfaceDesc m_desc;
    uint32_t m_blocks[3] = {}, m_tableSize = 0, m_maxBlocks = 0;
    render::ComPtr<ID3D12Resource> m_table, m_scan, m_density, m_counters, m_info, m_blockTris, m_vertices, m_velocities, m_cases, m_dispatch, m_draw;
    render::ComPtr<ID3D12Resource> m_basinTable;  // W3 seam: 64 basin records of 48 B (FluidBasin.hlsl)
    render::ComPtr<ID3D12CommandSignature> m_signature;
    bool m_casesUploaded = false;
    bool m_recorded = false;  // FluidTail: the first record retires the whole capacity
    render::ComPtr<ID3D12Resource> m_caseUpload;
};
} // namespace unx::water
