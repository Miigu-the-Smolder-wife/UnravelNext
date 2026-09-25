#pragma once
// The depth raster service's request (S -> V, INTERFACES_KO.md 5.3). Split from Frame.h (v1.42); Frame.h still includes
// it.
#include "unx/core/Math.h"
#include "unx/render/GraphTypes.h"
#include "unx/scene/SceneData.h"

#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
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
    TextureRef depthTarget;                        // hardware depth (D32_FLOAT or D16_UNORM); invalid when pixelKernel
                                                   // writes storage (then no render target and no depth: UAV-only
                                                   // raster, 1 sample)
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
    // Tile atlas (v1.32, S request 20260925_S_vsm_depth_atlas.md; needs tileLocal, cullMask and depthTarget): every set
    // tile is drawn on its own into its slot of the atlas 'depthTarget' (hardware depth, D32_FLOAT or D16_UNORM: the
    // requester picks per request, e.g. VSM D16 while the sun's zenith angle is below 76 degrees). The slot of tile i of
    // a view (bit i of its mask) is word cullMaskOffset * 32 + i of 'atlasSlots' (raw buffer, one uint per mask bit);
    // it sits at pixel (slot % atlasTilesPerRow, slot / atlasTilesPerRow) * cullTilePx of the atlas. The tile's pixels
    // move to the slot's by a whole-pixel shift in clip space (depth and perspective unchanged; the shift's float
    // rounding is below the rasteriser's 1/256 px snap), clipped to the tile, so fragments land only inside the slots of
    // set tiles. The views' viewport positions are ignored (their sizes give the tile grids); the pass viewport is the
    // whole atlas and a pixel kernel (optional; [earlydepthstencil] runs it after the depth test) sees atlas pixels.
    BufferRef atlasSlots;
    uint32_t atlasTilesPerRow = 0;
    // Coverage mode (v1.26; S's VSM transmittance layer): conservative raster of band B clusters only, the pixel kernel
    // (compiled with DEPTH_RASTER_COVERAGE 1) gets the exact area, mask and centroid depth per texel
    // (depthRasterCoverage, DepthRaster.hlsli). Needs a pixel kernel and no depth target. Bands are judged in each
    // view's texels (RasterView::lodPixelsPerMetre): A >= 1.5 texels, B 0.25..1.5, C < 0.25 (the requester's brick march).
    bool coverage = false;
    // Which bands a request draws (1 = A, 2 = B, 4 = C; default all, every band as depth). A transmittance-layer VSM
    // draws A as depth, B in coverage mode and marches C.
    uint32_t bands = 7;
};
} // namespace unx::render
