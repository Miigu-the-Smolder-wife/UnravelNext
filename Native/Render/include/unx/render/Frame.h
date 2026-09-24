#pragma once
// Frame contract between the render tracks (INTERFACES_KO.md 5). Core owns this file; tracks read it and fill the
// resources they own. Adding a field or service goes through the interface-change procedure (INTERFACES_KO.md 0).
#include "unx/core/Config.h"
#include "unx/core/Math.h"
#include "unx/render/Device.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/scene/SceneData.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
class GpuScene;

// A camera view. The main view comes from the scene camera; R builds planar-reflection views (5.4).
struct ViewDesc
{
    gpu::ViewKind kind = gpu::ViewKind::Main;
    uint32_t width = 0, height = 0;     // render target size of this view
    float4x4 view, proj, viewProj, prevViewProj, invViewProj;
    float3 position{};
    float nearPlane = 0.05f;
    float verticalFov = 1.0471976f;
    float4 clipPlane{};                 // world plane (xyz normal, w offset); keep dot(n,p) + w >= 0; zero = none.
                                        // V honours it with SV_ClipDistance0 and in cluster culling.
    bool mirrored = false;              // reflection views: front faces wind clockwise (V swaps cull mode)
    float ev100 = 14.0f;

    // Main view from a scene camera (reversed-Z infinite projection, Math.h).
    static ViewDesc fromCamera(const scene::Camera& camera, uint32_t width, uint32_t height, const float4x4& prevViewProj);
    // Mirror of 'mainView' across 'plane' (world), cropped to pixel rectangle 'region' of the main view (the planar
    // reflector's screen bounds, A_r): off-centre projection, oblique clip plane, same exposure. Owner of use: R.
    static ViewDesc planarReflection(const ViewDesc& mainView, float4 plane, uint32_t regionX, uint32_t regionY, uint32_t regionWidth, uint32_t regionHeight);
};

// Per-view products (graph resources of the current frame). Producer in brackets.
struct ViewResources
{
    ViewDesc view;
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;  // root CBV b1 for passes of this view [core]
    TextureRef depth;              // D32_FLOAT reversed Z                                   [V]
    TextureRef visId;              // R32_UINT (VisBuffer.hlsli)                             [V]
    BufferRef visibleClusters;     // gpu::VisibleCluster list indexed by the vis id         [V]
    TextureRef hiz;                // R32_FLOAT, half resolution, full mip chain (max-depth) [V]
    BufferRef coverageFragments;   // coverage layer fragments, sorted per pixel            [V]
    TextureRef coverageHeads;      // R32_UINT per pixel: first fragment | count << 24       [V]
    TextureRef gbuffer;            // RG32_UINT (GBuffer.hlsli)                              [M]
    TextureRef shadowVisibility;   // R32_UINT, 4 light slots x 8 bit (7.3)                 [S]
    TextureRef screenProbes;       // GI screen probes (main view only)                     [R]
    TextureRef reflection;         // RGBA16F reflection radiance + weight (main view only) [R]
    TextureRef color;              // final colour target of this view                      [M]
};

// View-independent products of the current frame. Persistent state (VSM pool, GI cache, TLAS) is imported into the
// graph each frame by its owner.
struct FrameResources
{
    TextureRef transmittanceLut, multiScatterLut, skyViewLut, aerialPerspective;  // [S]
    TextureRef vsmPool;            // physical page pool                                    [S]
    BufferRef vsmPageTable;        //                                                       [S]
    TextureRef froxels;            // scattering/transmittance volume                       [S]
    BufferRef froxelLights;        // per-froxel light lists (7.4)                          [S]
    BufferRef tlasStatic, tlasDynamic;  // acceleration structures                          [R]
    BufferRef giCache;             // world radiance cache                                  [R]
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
};

// S -> V: rasterise shadow-casting clusters into depth-like targets through V's cluster pipeline (cull, LOD, deform,
// mesh shader). V owns geometry; the requester owns the output: either hardware depth into 'depthTarget', or its own
// pixel kernel (e.g. atomic depth into paged storage) with the extra resources it declares.
struct RasterView
{
    float4x4 viewProj;
    uint32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;  // in the target
    float lodPixelsPerMetre = 0;   // screen-space scale for LOD selection (texels per metre at distance 1)
    uint32_t userData = 0;         // passed to the pixel kernel (e.g. clipmap level / page group)
};

struct DepthRasterRequest
{
    std::string name;                              // pass names: "<name>.<step>"
    std::vector<RasterView> views;
    uint32_t instanceMask = scene::InstanceCastShadow;  // instances with (flags & mask) != 0
    TextureRef depthTarget;                        // hardware depth (D32); invalid when pixelKernel writes storage
    std::string pixelKernel;                       // requester's pixel shader kernel; empty = depth only. Inputs:
                                                   // float4 position : SV_Position, nointerpolation uint userData : USERDATA
    std::vector<std::pair<TextureRef, Use>> textureUses;  // resources the pixel kernel touches
    std::vector<std::pair<BufferRef, Use>> bufferUses;
    uint32_t pixelConstants[16] = {};              // root constants 16..31 for the pixel kernel
    bool conservative = false;
};

struct FramePassContext;

// Cross-track services, provided by core (unx_frame) so modules never link each other.
struct FrameServices
{
    std::function<void(FramePassContext&, const DepthRasterRequest&)> rasterizeDepth;  // [V]
    // Records V -> M(resolve) -> S(shadow visibility) -> M(shading) for a secondary view and returns its products;
    // color is linear radiance x exposure in RGBA16F, sized view.width x view.height. Used by R for planar mirrors.
    std::function<ViewResources(FramePassContext&, const ViewDesc&)> renderView;
};

struct FramePassContext
{
    Device& device;
    RenderGraph& graph;
    ShaderLibrary& shaders;
    const QualityConfig& quality;
    GpuScene& scene;
    const FrameContext& frame;
    FrameResources& resources;
    FrameServices& services;
    // Frame constants for a view (allocates a 1 KB slot of this frame): bind with PassContext::bindFrameConstants.
    std::function<D3D12_GPU_VIRTUAL_ADDRESS(const ViewDesc&)> frameConstantsFor;
};
} // namespace unx::render
