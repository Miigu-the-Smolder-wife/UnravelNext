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
#include <memory>
#include <string>
#include <unordered_map>
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
    // Planar reflection views (v1.22, R request): which pixels are mirror pixels. R8_UINT, width x height, nonzero =
    // mirror pixel (drawn); invalid = every pixel. V culls clusters over 8 x 8 tiles without mirror pixels and fills
    // the other pixels' depth with the nearest value before the raster (they stay VIS_NONE); M and S skip them.
    TextureRef planarMask;              // [R]
    TextureRef planarTileMask;          // optional R8_UINT ceil(W/8) x ceil(H/8), nonzero = the tile has mirror [R]
                                        // pixels (M and S tile classification in one load)

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
    TextureRef hiz;                // R32_FLOAT, full mip chain: texel (i, j) of mip m = farthest   [V]
                                   // depth (minimum reversed-Z value) of the pixels [i 2^(m+1), ...);
                                   // valid mip size ceil(W / 2^(m+1)) x ceil(H / 2^(m+1)) inside a
                                   // power-of-two allocation (texels beyond it are undefined)
    BufferRef coverageFragments;   // coverage layer fragments, sorted per pixel            [V]
    TextureRef coverageHeads;      // R32_UINT per pixel: first fragment | count << 24       [V]
    TextureRef gbuffer;            // RG32_UINT (GBuffer.hlsli)                              [M]
    TextureRef shadowVisibility;   // R32_UINT, 4 light slots x 8 bit (7.3)                 [S]
    TextureRef shadowOverflowTiles;  // R32_UINT ceil(W/8) x ceil(H/8) (main view, 7.3, v1.20): [S]
                                     // 0 = no shadow-casting light past the third in the tile,
                                     // 0xFFFFFFFF = over capacity (fallback list), else 1 + the
                                     // tile's block start word in shadowOverflow
    BufferRef shadowOverflow;      // raw: per overflow tile 64 pixel words (count << 24 | run  [S]
                                   // start) + runs of 8-bit visibilities, list order (7.3)
    BufferRef shadowOverflowFallbackTiles;  // raw: word 0 count, words 1..3 DispatchIndirect  [S]
                                            // args (count, 1, 1), words 4.. tiles (y << 16 | x)
    BufferRef froxelLights;        // this view's froxel light lists (7.4; v1.22): main view =      [S]
                                   // FrameResources::froxelLights, planar views: S shadowVisibility
    TextureRef airVolume;          // this view's air volume (v1.15 layout; v1.22): main view =     [S]
                                   // FrameResources::aerialPerspective; planar views integrate from
                                   // the mirror plane on (the main view's mirror pixel has the rest)
    TextureRef screenProbes;       // GI screen probes (main view only)                     [R]
    TextureRef screenProbeMaps;    // atlas of the K-path radiance maps of the cache entries  [R]
                                   // the screen probes use, hardware-filterable (M: SrvCompute; R's
                                   // ScreenProbes.hlsli defines the layout; v1.13)
    TextureRef reflection;         // RGBA16F reflection radiance + weight (main view only) [R]
    TextureRef reflectionLobeTiles;  // R8_UNORM ceil(W/8) x ceil(H/8): min over the tile's      [M]
                                     // surface pixels of reflectionLobeHalfAngle(r, NoV) / pi
                                     // (Reflection.hlsli; sky-only tile = 1); R skips ray
                                     // classification in tiles whose minimum is K-path wide
    TextureRef color;              // final colour target of this view                      [M]
};

// View-independent products of the current frame. Persistent state (VSM pool, GI cache, TLAS) is imported into the
// graph each frame by its owner.
struct FrameResources
{
    TextureRef transmittanceLut, multiScatterLut, skyViewLut;  // [S]
    TextureRef aerialPerspective;  // the air volume (froxels(), v1.15): Texture3D RGBA16F      [S]
                                   // gridX x gridY x 3(S+1) on the froxel grid, part 0 in-scattering
                                   // camera -> node (x exposure; atmosphere, caster-shadowed air, local
                                   // lights), part 1 optical depth, part 2 sun transmittance at the node;
                                   // read with atmosphereAerial / atmosphereAirView (Atmosphere.hlsli)
    BufferRef vsmPool;             // physical page pool (raw buffer; v1.18)                [S]
    BufferRef vsmPageTable;        //                                                       [S]
    BufferRef vsmBlocks;           // per-page block hierarchy (persistent; v1.18)          [S]
    BufferRef vsmSearchBound;      // blocker-search bound grid of this frame (v1.18)       [S]
    uint32_t vsmConstants = UINT32_MAX;  // CBV descriptor of this frame's VSM constants    [S]
                                         // (upload ring, not a graph resource; v1.18). With
                                         // the four buffers: ShadowSrvs (ShadowVisibility.hlsli),
                                         // filled by shadowPages for shadowSunVisibilityAt (R)
    uint32_t vsmLocalLights = UINT32_MAX;  // SRV descriptors of this frame's local-light shadow [S]
    uint32_t vsmSlotOfLight = UINT32_MAX;  // records (VsmLocalLight, 48 B x shadow slots) and the
                                           // scene light -> shadow slot table (uint, 0xFFFF = none)
                                           // (upload ring; v1.19): ShadowSrvs.lights / .pad0 for
                                           // shadowVisibilityDirect
    TextureRef froxels;            // the same air volume as aerialPerspective (v1.15)      [S]
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
    uint32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;  // in the target (up to 16384^2)
    // LOD scale. Perspective viewProj: texels per metre at distance 1 (focal length in texels). Orthographic viewProj
    // (last row 0,0,0,1; V detects it): texels per metre, independent of distance.
    float lodPixelsPerMetre = 0;
    uint32_t userData = 0;         // passed to the pixel kernel (e.g. clipmap level / page group)
    uint32_t cullMaskOffset = UINT32_MAX;  // first uint32 word of this view's tile mask in DepthRasterRequest::cullMask;
                                           // UINT32_MAX = no mask (the whole viewport is rasterised)
};

struct DepthRasterRequest
{
    std::string name;                              // pass names: "<name>.<step>"
    std::vector<RasterView> views;
    uint32_t instanceMask = scene::InstanceCastShadow;  // instances with (flags & mask) != 0
    TextureRef depthTarget;                        // hardware depth (D32); invalid when pixelKernel writes storage
                                                   // (then no render target and no depth: UAV-only raster, 1 sample)
    std::string pixelKernel;                       // requester's pixel shader kernel; empty = depth only. Its input is
                                                   // struct DepthRasterPixel (Passes/Visibility/DepthRaster.hlsli); it
                                                   // calls depthRasterCovered(p) first (alpha-tested materials)
    std::vector<std::pair<TextureRef, Use>> textureUses;  // resources the pixel kernel touches
    std::vector<std::pair<BufferRef, Use>> bufferUses;
    uint32_t pixelConstants[16] = {};              // root constants 16..31 for the pixel kernel
    bool conservative = false;
    D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;   // default both faces (shadows); BACK culls back faces of one-sided
                                                   // materials only (two-sided materials are never culled)
    // Tile mask (performance only; the pixel kernel still decides what it writes): raw buffer, per view
    // ceil(viewportWidth / cullTilePx) x ceil(viewportHeight / cullTilePx) bits, row major, bit i = bit (i & 31) of word
    // cullMaskOffset + (i >> 5); 1 = the tile needs rasterisation. Written on the GPU earlier in the same frame. V skips
    // clusters (and meshlet triangles) whose viewport rectangle covers no set bit.
    BufferRef cullMask;
    uint32_t cullTilePx = 0;
    // Tile-local raster (v1.7; needs cullMask): each cluster is drawn once per run of set tiles it covers (one draw
    // when all tiles under it are set), clipped to that run's rectangle, so the rasteriser makes fragments only inside
    // set tiles (fragments = sum of triangle area inside set tiles). Pixel positions, depth and DepthRasterPixel are
    // the same as without it. For sparse masks over large viewports (VSM dirty pages in a 16384^2 level).
    bool tileLocal = false;
};

struct FramePassContext;

// Persistent state of a track (history buffers, pools, caches) owned by the FrameRenderer: created on first use,
// destroyed with the renderer after the GPU is idle. Keys are "<track>.<name>"; one key always holds one type.
class TrackState
{
public:
    template <typename T>
    T& get(const std::string& key)
    {
        std::shared_ptr<void>& p = m_entries[key];
        if (!p) p = std::make_shared<T>();
        return *static_cast<T*>(p.get());
    }
    void clear() { m_entries.clear(); }

private:
    std::unordered_map<std::string, std::shared_ptr<void>> m_entries;
};

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
    TrackState* trackState = nullptr;  // FrameRenderer's; null in contexts built without a renderer
    uint32_t framesInFlight = 2;       // per-frame CPU-written resources of frame f live in slot f % framesInFlight;
                                       // the caller waits for that slot's fences before recording frame f

    template <typename T>
    T& state(const std::string& key)
    {
        if (!trackState) fail("FramePassContext::state('%s'): no track state in this context", key.c_str());
        return trackState->get<T>(key);
    }
};
} // namespace unx::render
