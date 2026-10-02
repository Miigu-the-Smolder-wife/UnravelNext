#pragma once
// The depth raster service's request (S -> V, INTERFACES_KO.md 5.3). Split from Frame.h (v1.42); Frame.h still includes
// it.
#include "unx/core/Math.h"
#include "unx/render/GraphTypes.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
// V -> requester (INTERFACES 3.6): the newest frame in which a depth raster run (DepthRasterRequest::name) overflowed its
// lists, and its overflow bits - geometry was dropped from what that frame drew. V writes it when the run's statistics
// come back (framesInFlight frames later); requesters that keep what a run drew (S's page cache) redraw it. TrackState
// key kDepthRasterOverflowKey.
struct DepthRasterOverflow
{
    uint64_t frame = UINT64_MAX;
    uint32_t bits = 0;
};
using DepthRasterOverflows = std::map<std::string, DepthRasterOverflow>;
inline const char* kDepthRasterOverflowKey = "depthRaster.overflows";

// The most views one request holds (the cull work items carry the view in 16 bits; a run's views are one upload chunk).
constexpr uint32_t kDepthRasterMaxViews = 4096;

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
    // Instance batch: only the scene instances [instanceFirst, instanceEnd) are drawn into this view (instanceEnd 0:
    // every instance). A requester whose view can reach more clusters than a run's lists hold (a forest's million trees
    // in one shadow level) draws the view in several requests, a range of the instances each.
    uint32_t instanceFirst = 0, instanceEnd = 0;
    // The smallest caster the view draws: an instance whose bounding sphere projects to under this many of the view's
    // texels in radius (radius x lodPixelsPerMetre; perspective: over the distance to the sphere's nearest point) is not
    // drawn into it. 0: every instance. A shadow level coarser than a caster gets less than a texel from it.
    float minInstanceTexels = 0;
    // Which instances the view draws: 0 every instance; 1 the instances that keep their place and shape between frames
    // (uploaded with the scene and not movable: gpu::instanceMovable); 2 the others (movable ones, and every run-time and
    // GPU-written instance). A view of set 1 and a view of set 2 with one projection draw every instance exactly once
    // between them (S's static / dynamic shadow pages).
    uint32_t instanceSet = 0;
    // First word of the view's tile slots in DepthRasterRequest::atlasSlots (tile i: word atlasSlotOffset + i);
    // UINT32_MAX = cullMaskOffset * 32, the layout of the mask. Two views with different masks over the same tiles (S's
    // static and movable casters' views of one face) can then share one set of slots.
    uint32_t atlasSlotOffset = UINT32_MAX;
    // The view is tested against the request's tile occluders (DepthRasterRequest::tileOccluders).
    bool tileOccluders = false;
    // ... in two phases (DepthRasterRequest::tileGuess): for a view whose tiles' occluders are what the view itself draws.
    bool tileTwoPhase = false;
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
    // Graph resources whose bindless view index the pixel kernel needs (v2: the surface cache's cluster capture). When
    // the raster pass executes, V writes each one's index into pixelConstants[word]: its UAV where the uses above declare
    // it written (UavGraphics), its SRV otherwise (one not named in the uses is declared SrvGraphics). The indices of
    // graph resources exist only then: a requester need not keep a buffer of its own to pass them.
    struct PixelView
    {
        uint32_t word = 0;
        TextureRef texture;  // one of the two
        BufferRef buffer;
    };
    std::vector<PixelView> pixelViews;
    // The pixel kernel's interpolated surface frame (v2): the mesh kernel also exports each vertex's world normal and
    // tangent (deformed like its position: skin, wind, morphs), and the kernel - compiled with DEPTH_RASTER_NORMALS 1
    // before DepthRaster.hlsli - reads DepthRasterPixel::normal and ::tangent (w: the bitangent's sign). Without it a
    // kernel has only the triangle's normal, from its depth's steps.
    bool pixelNormals = false;
    // Render targets of the pixel kernel (v2: SV_Target0 .., at most 4; needs a pixel kernel). With depthTarget the depth
    // test settles which fragment's outputs a pixel keeps, so one run draws depth and attributes - a kernel that wrote
    // them through UAVs needed a depth run first. In atlas mode they are atlases laid out like depthTarget. The requester
    // clears them. colorMultiply: every target's rgb = stored x written, a = max(stored, written) (a transmittance
    // product with the nearest depth beside it: S's see-through casters). depthWrite false: tested against depthTarget,
    // nothing written to it.
    std::vector<TextureRef> colorTargets;
    bool colorMultiply = false;
    bool depthWrite = true;
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
    // Tile occluders (with the tile atlas and cullTilePx 128; S's page HZB of the static casters): a raw buffer of
    // 341 floats per atlas slot - the farthest device depth already stored in the slot over each of its 16 x 16 blocks of
    // 8 px, then over its blocks of 16, 32, 64 px and the whole tile (offsets 0, 256, 320, 336, 340; 0 = a pixel with
    // nothing stored, which hides nothing). In a view with RasterView::tileOccluders, an instance, hierarchy node or
    // cluster whose bounding sphere is farther than that depth everywhere under it, in every set tile under it, is not
    // drawn: its fragments would lose the depth test against what the requester merges in afterwards. Spheres over more
    // than 2 x 2 tiles are not tested. The cull kernels read both buffers through the views' records, so the requester
    // gives their persistent raw SRVs (bindless indices) beside the graph handles.
    BufferRef tileOccluders;
    uint32_t tileOccludersSrv = UINT32_MAX, atlasSlotsSrv = UINT32_MAX;
    // Tile occluders in two phases (views with RasterView::tileTwoPhase; the reference's two-pass occlusion of its shadow
    // views): the tiles' occluders are what the views themselves draw, so they do not exist when the run starts. Phase 1
    // tests against a guess per tile - 'tileGuess', a raw buffer of two words per mask bit (as atlasSlots: word pair
    // cullMaskOffset * 32 + i): { the atlas slot whose occluder record stands for the tile, UINT32_MAX = none;
    // shift | x << 8 | y << 20: the tile's pixel p is pixel (x, y) + (p >> shift) of that slot } (S: a kept coarser page
    // over the same ground) - and draws what the guess does not hide; V then calls 'buildTileOccluders', in which the
    // requester records the pass that rebuilds tileOccluders for the slots just drawn; phase 2 tests what phase 1
    // rejected against them and draws what they do not hide. A guess only decides in which phase something is drawn:
    // what is left out lies behind the depth phase 1 stored, whatever the guess said.
    BufferRef tileGuess;
    uint32_t tileGuessSrv = UINT32_MAX;
    std::function<void()> buildTileOccluders;
    // Small casters as proxies (depth-only requests: no pixel kernel; whole viewports or the tile atlas). A view with
    // RasterView::minInstanceTexels leaves out the instances whose bounds project under it; with proxies each of those
    // that belongs to V's instance chunks (the static instances) is drawn instead as one square facing the view at its
    // bounds' centre, of area proxyCoverage x its bounding disc's - the share of that disc its silhouette fills. A square
    // under a texel covers a texel centre as often as its area is of a texel: a level coarser than its casters keeps
    // their shadow as a density, at two triangles a caster, and a chunk whose every member is that small is not culled
    // member by member. Dynamic, skinned and run-time instances under the minimum stay left out.
    bool proxies = false;
    float proxyCoverage = 0.5f;
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
