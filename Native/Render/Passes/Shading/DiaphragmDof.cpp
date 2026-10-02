// Diaphragm depth of field of M (shading.dof_diaphragm; DepthOfField.cpp's depthOfField hands over to it): the
// reference's lens blur (its PostProcess/DiaphragmDOF.cpp and Shaders DiaphragmDOF/*) on this renderer's kernels, at
// the view's (internal) resolution before the temporal upscale, which accumulates it. The lens is the one the octave
// path takes - FrameContext::lensAperture (diameter, m), lensFocus (m), the view's projection as the sensor: a depth
// z has the circle of confusion f_px A (1 - z_f / z) / (2 z_f) pixels in radius - here in half-resolution pixels, signed
// (negative in front of the focus), limited to shading.dof_diaphragm_max_*_radius of the view's width.
// The passes (DdofCommon.hlsli has the layers):
//   m.dof.d.setup        colour and radius at half resolution (DdofSetup.hlsl);
//   m.dof.d.stabilize    a jittered view only: that picture accumulated on the unjittered grid (DdofStabilize.hlsl);
//   m.dof.d.tiles.*      per 8 x 8 of it the radii in reach: flatten, then dilate by the widest blur (DdofTiles.hlsl);
//   m.dof.d.quarter,     its coarser levels for the wide kernels, and the bright wide-radius pixels taken out of it into
//   m.dof.d.reduce       the two sprite lists (DdofSetup.hlsl STEP=1, DdofReduce.hlsl);
//   m.dof.d.bokeh        the diaphragm's shape as tables, when it has blades (DdofBokehLut.hlsl);
//   m.dof.d.gather.*     foreground, hole filling, slight, background at half resolution (DdofGather.hlsl);
//   m.dof.d.postfilter.* the foreground's and the background's 3 x 3 median (DdofPostfilter.hlsl);
//   m.dof.d.scatter.*    the lists' sprites added to those two layers (DdofScatter.ms/.ps.hlsl);
//   m.dof.d.recombine    the full-resolution gather of the slight radii and the layers composed (DdofRecombine.hlsl).
// The lens's shape (the reference's FPhysicalCocModel; DdofCommon.hlsli, DdofScatter.hlsli): the anamorphic squeeze
// (the bokeh narrower by it: every kernel and sprite), the Petzval stretch (the wide gathers and the sprites), the
// barrel's and the matte box's cut of the sprites, the depth blur term of the radius. The reference's defaults for
// these are in its Engine module, which the reference checkout does not have: the quality file's are as remembered.
// Not ported: the dynamic radius offset, the alpha channel.
#include "unx/shading/DepthOfField.h"

#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/shading/Upscale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace unx::render::shading
{
namespace
{
constexpr uint32_t kTile = 8, kMaxLevels = 4, kSpritesPerGroup = 32, kLut = 32;
constexpr uint32_t kScatterHeader = 16, kScatterBytes = 80;  // (DdofCommon.hlsli DDOF_SCATTER_*)
constexpr float kPi = 3.14159265358979f;
constexpr float kMinScatterRadius = 3.0f;  // (the reference's kMinScatteringCocRadius)
constexpr uint32_t kNone = 0xFFFFFFFFu;

uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// The prefilter's history: the stabilized half-resolution colour and radius of the last frame (two textures in turn).
struct DiaphragmState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> history[2];
    uint32_t width[2] = {}, height[2] = {};  // per slot: the size of the frame that wrote it (dynamic resolution: the
                                             // internal size is each frame's own; the kernel reads the history by UV)
    uint32_t parity = 0;
    uint64_t written = ~0ull;  // the frame history[parity] is of
    ~DiaphragmState()
    {
        if (!device) return;
        for (ComPtr<ID3D12Resource>& h : history)
            if (h) device->deferRelease(h);
    }
    // 'slot': the one this frame writes - recreated when its size is not this frame's; the other keeps its own.
    void ensure(Device& d, uint32_t w, uint32_t h, uint32_t slot)
    {
        const bool all = !history[0];
        if (!all && width[slot] == w && height[slot] == h) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (uint32_t k = 0; k < 2; ++k)
        {
            if (!all && k != slot) continue;
            if (history[k]) d.deferRelease(history[k]);
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(history[k].ReleaseAndGetAddressOf())),
                  "M depth of field prefilter history");
            history[k]->SetName(k ? L"M depth of field prefilter history 1" : L"M depth of field prefilter history 0");
            width[k] = w;
            height[k] = h;
        }
        if (all) written = ~0ull;
    }
};

// The diaphragm (the reference's FBokehModel): no blades - a disc; straight blades - a polygon; a lens stopped down from
// a wider full aperture - blades that are arcs. Radii are scaled so the shape's area is the unit disc's.
struct Diaphragm
{
    uint32_t blades = 0;
    float circumscribed = 1, incircle = 1, rotation = 0;
    float bladeRadius = 0, bladeOffset = 0;  // (0: straight)
};
Diaphragm diaphragmOf(int64_t blades, float aperture, float fullAperture)
{
    Diaphragm d;
    if (blades < 4 || (fullAperture > 0 && fullAperture <= aperture)) return d;  // (wide open: the lens's own circle)
    d.blades = (uint32_t)std::min<int64_t>(blades, 16);
    const float coverage = kPi / (float)d.blades;
    const float triangle = std::cos(coverage) * std::sin(coverage);  // (of the polygon inscribed in the unit circle)
    if (fullAperture <= 0)
    {
        d.circumscribed = 1.0f / std::sqrt((float)d.blades * triangle / kPi);
        d.incircle = d.circumscribed * std::cos(coverage);
        return d;
    }
    // a blade is an arc of the full aperture's circle seen through the stopped-down opening
    const float radius = fullAperture / aperture;  // (for a circumscribed radius of 1: f-stop over the widest f-stop)
    const float visible = std::asin(std::sin(coverage) / radius);
    const float offset = radius * std::cos(visible) - std::cos(coverage);
    const float bladeTriangle = radius * radius * std::cos(visible) * std::sin(visible);
    const float area = (float)d.blades * (triangle + radius * radius * visible - bladeTriangle);
    const float scale = std::sqrt(kPi / area);
    const float pivotX = 0.5f * (radius - 1.0f), pivotY = std::sqrt(radius * radius - pivotX * pivotX);
    d.rotation = std::atan2(pivotX, pivotY);
    d.bladeRadius = scale * radius;
    d.bladeOffset = scale * offset;
    d.circumscribed = scale;
    d.incircle = scale * (radius - offset);
    return d;
}

double numberOr(FramePassContext& fc, const char* key, double fallback) { return fc.quality.has(key) ? fc.quality.number(key) : fallback; }
int64_t integerOr(FramePassContext& fc, const char* key, int64_t fallback) { return fc.quality.has(key) ? fc.quality.integer(key) : fallback; }
bool booleanOr(FramePassContext& fc, const char* key, bool fallback) { return fc.quality.has(key) ? fc.quality.boolean(key) : fallback; }
} // namespace

bool diaphragmDepthOfFieldOn(FramePassContext& fc)
{
    return fc.quality.has("shading.dof_diaphragm") && fc.quality.boolean("shading.dof_diaphragm");
}

BufferRef diaphragmDepthOfField(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height, hw = (w + 1) / 2, hh = (h + 1) / 2;
    const uint32_t tilesX = (hw + kTile - 1) / kTile, tilesY = (hh + kTile - 1) / kTile;
    const uint32_t groupsX = tilesX, groupsY = tilesY;  // (a gather group is a tile)
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef depth = view.depth;

    // the lens, in half-resolution pixels (DdofCommon.hlsli ddofCoc)
    const float fpx = 0.5f * (float)h * view.view.proj.m[1][1];
    const float infinity = 0.25f * fpx * fc.frame.lensAperture / fc.frame.lensFocus;
    const float maxForeground = (float)numberOr(fc, "shading.dof_diaphragm_max_foreground_radius", 0.025) * (float)hw;
    const float maxBackground = (float)numberOr(fc, "shading.dof_diaphragm_max_background_radius", 0.025) * (float)hw;
    const float lens[4] = { infinity, infinity * fc.frame.lensFocus / view.view.nearPlane, -maxForeground, maxBackground };
    // the depth blur (the reference's DepthOfFieldDepthBlurRadius - half-resolution pixels of a 1920-wide view - and
    // DepthOfFieldDepthBlurAmount, the distance in km at which it is half): { radius, exponent x near plane }
    const float depthBlurKm = (float)numberOr(fc, "shading.dof_diaphragm_depth_blur_km", 1.0);
    const float depthBlurRadius =
        depthBlurKm > 0 ? std::max((float)numberOr(fc, "shading.dof_diaphragm_depth_blur_radius", 0.0), 0.0f) * 2.0f * (float)hw / 1920.0f : 0.0f;
    const float depthBlur[2] = { depthBlurRadius, depthBlurRadius > 0 ? view.view.nearPlane / (depthBlurKm * 1000.0f) : 0.0f };
    // (a surface at the lens is as blurred as the limit lets it)
    const float maxBlur = std::max(std::min(std::max(infinity, depthBlurRadius), maxBackground), maxForeground);

    // the lens's shape (DdofCommon.hlsli, DdofScatter.hlsli)
    const float squeeze = (float)std::clamp(numberOr(fc, "shading.dof_diaphragm_squeeze", 1.0), 0.1, 10.0);
    const float aspect = (float)w / (float)h;
    const float petzval[4] = { (float)numberOr(fc, "shading.dof_diaphragm_petzval", 0.0),
                               (float)std::clamp(numberOr(fc, "shading.dof_diaphragm_petzval_falloff", 1.0), 0.0, 100.0),
                               std::max((float)numberOr(fc, "shading.dof_diaphragm_petzval_box_x", 0.0), 0.0f),
                               std::max((float)numberOr(fc, "shading.dof_diaphragm_petzval_box_y", 0.0), 0.0f) };
    const float petzvalCorner = std::max((float)numberOr(fc, "shading.dof_diaphragm_petzval_box_radius", 0.0), 0.0f);
    // the barrel (a tube in front of the aperture, never narrower than the aperture and 5 mm) and the matte box's flags
    // on its rim: a flag at 'roll' around the axis (0: the picture's right, 90: its top), 'length' long, tilted by
    // 'pitch' from the axis (0: straight ahead, the barrel's wall carried on; below: closing over the opening)
    const float apertureRadius = 0.5f * fc.frame.lensAperture;
    const float barrelRadius = std::max((float)numberOr(fc, "shading.dof_diaphragm_barrel_radius", 0.05), apertureRadius + 0.005f);
    const float barrelLengthSet = std::max((float)numberOr(fc, "shading.dof_diaphragm_barrel_length", 0.0), 0.0f);
    float flags[3][4] = {};  // (DdofScatter.hlsli P[6..8])
    bool matteBox = false;
    for (int i = 0; i < 3; ++i)
    {
        const std::string key = "shading.dof_diaphragm_matte_box_" + std::to_string(i);
        const float length = (float)numberOr(fc, (key + "_length").c_str(), 0.0);
        if (!(length > 0)) continue;
        const float pitch = ((float)std::clamp(numberOr(fc, (key + "_pitch").c_str(), 0.0), -89.0, 89.0) + 90.0f) * kPi / 180.0f;
        const float roll = (float)numberOr(fc, (key + "_roll").c_str(), 0.0) * kPi / 180.0f;
        flags[i][0] = std::cos(roll);
        flags[i][1] = std::sin(roll);
        flags[i][2] = barrelRadius - std::cos(pitch) * length;
        flags[i][3] = std::sin(pitch) * length;
        matteBox = true;
    }
    const float barrelLength = barrelLengthSet > 0 || matteBox ? barrelLengthSet : -1.0f;  // (< 0: no vignetting)
    const float tanHalf[2] = { 1.0f / (view.view.proj.m[0][0] * squeeze), 1.0f / view.view.proj.m[1][1] };
    const float focus = fc.frame.lensFocus;

    const uint32_t rings = (uint32_t)std::clamp<int64_t>(integerOr(fc, "shading.dof_diaphragm_rings", 5), 3, 5);
    const int64_t levelLimit = std::clamp<int64_t>(integerOr(fc, "shading.dof_diaphragm_max_levels", 4), 1, kMaxLevels);
    const uint32_t levels = (uint32_t)std::clamp<int64_t>((int64_t)std::ceil(std::log2(std::max(maxBlur * 0.5f / (float)rings, 1e-3f))), 1, levelLimit);
    const int64_t postfilter = integerOr(fc, "shading.dof_diaphragm_postfilter", 1);
    const uint32_t pairs = (uint32_t)std::clamp<int64_t>(integerOr(fc, "shading.dof_diaphragm_recombine_pairs", 16), 1, 32);
    const float spriteRatio = (float)std::clamp(numberOr(fc, "shading.dof_diaphragm_scatter_max_sprite_ratio", 0.1), 0.0, 1.0);
    const float minScatterRadius = std::max((float)numberOr(fc, "shading.dof_diaphragm_scatter_min_radius", 3.0), kMinScatterRadius);
    const bool scatter = booleanOr(fc, "shading.dof_diaphragm_scatter", true) && spriteRatio > 0 && maxBlur > minScatterRadius;
    const bool occlusion = scatter && booleanOr(fc, "shading.dof_diaphragm_scatter_occlusion", true);
    // (the diaphragm: the frame's - a game's run-time setting, FrameContext::post - over the quality file's)
    const PostSettingsDesc& frameSettings = fc.frame.post;
    const int64_t blades = frameSettings.diaphragmBlades >= 0 ? frameSettings.diaphragmBlades : integerOr(fc, "shading.dof_diaphragm_blades", 0);
    const float fullAperture = std::isfinite(frameSettings.lensFullAperture) ? frameSettings.lensFullAperture
                                                                             : (float)numberOr(fc, "shading.dof_diaphragm_full_aperture", 0.0);
    const Diaphragm diaphragm = diaphragmOf(blades, fc.frame.lensAperture, fullAperture);
    const bool shaped = diaphragm.blades != 0;

    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");  // (zeroes a raw buffer's words)
    const BufferRef stats = g.createBuffer({ "m.dof.stats", 40, 0 });  // (the octave path's statistics: this path counts nothing)
    g.addPass("m.dof.d.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(stats), 10, 0, 0 };
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });

    // setup
    const TextureRef half = g.createTexture(TextureDesc{ "m.dof.d.half", hw, hh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    ID3D12PipelineState* setupPso = fc.shaders.compute("Passes/Shading/DdofSetup.STEP0");
    g.addPass("m.dof.d.setup", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(half, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[16] = { c.srv(src), c.srv(depth), c.uav(half), 0, w, h, hw, hh };
                  std::memcpy(&k[8], lens, 16);
                  k[12] = asUint(depthBlur[0]);
                  k[13] = asUint(depthBlur[1]);
                  k[14] = k[15] = 0;
                  c.cmd->SetPipelineState(setupPso);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
              });

    // the prefilter (a jittered view; the reference's r.DOF.TemporalAAQuality pass)
    TextureRef input = half;
    const bool prefilter = fc.trackState && upscaleActive(fc, view) && booleanOr(fc, "shading.dof_diaphragm_prefilter", true);
    if (prefilter)
    {
        DiaphragmState& s = fc.state<DiaphragmState>("M.dof.diaphragm");
        s.ensure(fc.device, hw, hh, s.parity ^ 1u);
        const bool valid = !fc.frame.upscale.reset && s.written != ~0ull && s.written + 1 == fc.frame.frameIndex;
        const TextureRef previous = g.importTexture(s.history[s.parity].Get(),
                                                    { "m.dof.d.stable (previous)", s.width[s.parity], s.height[s.parity], 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        s.parity ^= 1u;
        s.written = fc.frame.frameIndex;
        const TextureRef stable = g.importTexture(s.history[s.parity].Get(), { "m.dof.d.stable", hw, hh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const float jx = fc.frame.upscale.jitterX, jy = fc.frame.upscale.jitterY;
        const float weight = (float)numberOr(fc, "shading.dof_diaphragm_prefilter_weight", 0.04);
        const float4x4 prevViewProj = fc.frame.upscale.prevViewProj;
        // shading.dof_diaphragm_prefilter_velocity: the history is read where each pixel's own surface was - the
        // upscale's vectors, recorded here ahead of the upscale (upscaleMotion); false: by the camera's motion only
        const UpscaleMotion vectors = booleanOr(fc, "shading.dof_diaphragm_prefilter_velocity", true) ? upscaleMotion(fc, view) : UpscaleMotion{};
        const bool hasVectors = vectors.motion.valid() && vectors.depth.valid();
        const TextureRef motion = vectors.motion, surface = hasVectors ? vectors.depth : depth;  // (the depth the motion is taken at)
        ID3D12PipelineState* stabilizePso = fc.shaders.compute("Passes/Shading/DdofStabilize");
        g.addPass("m.dof.d.stabilize", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(half, Use::SrvCompute);
                      b.use(previous, Use::SrvCompute);
                      b.use(surface, Use::SrvCompute);
                      if (hasVectors) b.use(motion, Use::SrvCompute);
                      b.use(stable, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[32] = { c.srv(half), c.srv(previous), c.srv(surface), c.uav(stable), hw, hh, w, h, asUint(jx), asUint(jy), asUint(weight),
                                         (valid ? 1u : 0u) | (hasVectors ? 2u : 0u) };
                      for (int r = 0; r < 4; ++r)
                          for (int col = 0; col < 4; ++col) k[12 + 4 * r + col] = asUint(prevViewProj.m[r][col]);
                      k[28] = hasVectors ? c.srv(motion) : kNone;
                      k[29] = c.srv(surface);
                      k[30] = k[31] = 0;
                      c.cmd->SetPipelineState(stabilizePso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 32);
                      c.cmd->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
                  });
        input = stable;
    }

    // the radius tiles: flatten, then dilate by the widest blur (the reference's pass plan: up to three rings a pass, the
    // later passes stepping further; with more than one pass the widest radii first, then the narrowest under them)
    const DXGI_FORMAT tileFormatForeground = DXGI_FORMAT_R16G16_FLOAT, tileFormatBackground = DXGI_FORMAT_R16G16B16A16_FLOAT;
    struct Tiles
    {
        TextureRef foreground, background;
    };
    auto tileTextures = [&](const char* nameForeground, const char* nameBackground) {
        return Tiles{ g.createTexture(TextureDesc{ nameForeground, tilesX, tilesY, 1, 1, tileFormatForeground }),
                      g.createTexture(TextureDesc{ nameBackground, tilesX, tilesY, 1, 1, tileFormatBackground }) };
    };
    const Tiles flat = tileTextures("m.dof.d.tiles.flat foreground", "m.dof.d.tiles.flat background");
    ID3D12PipelineState* flattenPso = fc.shaders.compute("Passes/Shading/DdofTiles.STEP0");
    ID3D12PipelineState* dilatePso = fc.shaders.compute("Passes/Shading/DdofTiles.STEP1");
    g.addPass("m.dof.d.tiles.flatten", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(input, Use::SrvCompute);
                  b.use(flat.foreground, Use::UavCompute);
                  b.use(flat.background, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(input), c.uav(flat.foreground), c.uav(flat.background), 0, hw, hh, tilesX, tilesY };
                  c.cmd->SetPipelineState(flattenPso);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    // (the gather kernel's centre is shifted by up to a ring spacing; a squeeze under 1 widens the bokeh across)
    const float reach = (1.0f + 1.0f / ((float)rings + 0.5f)) * std::max(1.0f, 1.0f / squeeze);
    uint32_t dilates = 1, ringCount[3] = {}, ringStep[3] = {};
    {
        const uint32_t widest = (uint32_t)std::ceil(maxBlur * reach / (float)kTile);
        uint32_t covered = std::min(widest, 3u);
        ringCount[0] = std::max(covered, 1u);  // (always one ring: a small blur still reaches its neighbour)
        ringStep[0] = 1;
        for (uint32_t i = 1; i < 3 && widest > covered; ++i)
        {
            const uint32_t stepLimit = covered + 1;  // (a larger step would skip tiles)
            ringCount[i] = std::min(widest / stepLimit, 3u);
            ringStep[i] = std::min((widest - covered + ringCount[1] - 1) / ringCount[1], stepLimit);
            covered += ringCount[i] * ringStep[i];
            ++dilates;
        }
    }
    auto dilate = [&](const char* name, uint32_t mode, Tiles from, Tiles widestTiles, uint32_t count, uint32_t step) {
        const Tiles to = tileTextures("m.dof.d.tiles.dilated foreground", "m.dof.d.tiles.dilated background");
        g.addPass(name, QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(from.foreground, Use::SrvCompute);
                      b.use(from.background, Use::SrvCompute);
                      if (mode == 2)
                      {
                          b.use(widestTiles.foreground, Use::SrvCompute);
                          b.use(widestTiles.background, Use::SrvCompute);
                      }
                      b.use(to.foreground, Use::UavCompute);
                      b.use(to.background, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[12] = { c.srv(from.foreground), c.srv(from.background), c.uav(to.foreground), c.uav(to.background),
                                               tilesX, tilesY, count, step,
                                               mode, mode == 2 ? c.srv(widestTiles.foreground) : kNone, mode == 2 ? c.srv(widestTiles.background) : kNone, asUint(reach) };
                      c.cmd->SetPipelineState(dilatePso);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
                  });
        return to;
    };
    Tiles tiles = flat;
    if (dilates > 1)
    {
        Tiles widestTiles = flat;
        for (uint32_t i = 0; i < dilates; ++i) widestTiles = dilate("m.dof.d.tiles.dilate widest", 1, widestTiles, Tiles{}, ringCount[i], ringStep[i]);
        for (uint32_t i = 0; i < dilates; ++i) tiles = dilate("m.dof.d.tiles.dilate narrowest", 2, tiles, widestTiles, ringCount[i], ringStep[i]);
    }
    else tiles = dilate("m.dof.d.tiles.dilate", 0, flat, Tiles{}, ringCount[0], ringStep[0]);

    // the reduce: the gathers' levels (padded to whole groups), and the sprite lists
    const uint32_t pw = groupsX * 8, ph = groupsY * 8;
    TextureRef level[kMaxLevels];
    for (uint32_t l = 0; l < levels; ++l)
        level[l] = g.createTexture(TextureDesc{ "m.dof.d.level", pw >> l, ph >> l, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    for (uint32_t l = levels; l < kMaxLevels; ++l) level[l] = level[0];
    const uint32_t capacity = scatter ? std::max((uint32_t)(spriteRatio * 0.25f * (float)hw * (float)hh), kSpritesPerGroup) : 0u;
    const uint32_t qw = (hw + 1) / 2, qh = (hh + 1) / 2;
    TextureRef quarter;
    BufferRef listForeground, listBackground;
    if (scatter)
    {
        quarter = g.createTexture(TextureDesc{ "m.dof.d.quarter", qw, qh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        listForeground = g.createBuffer({ "m.dof.d.scatter list foreground", kScatterHeader + (uint64_t)capacity * kScatterBytes, 0 });
        listBackground = g.createBuffer({ "m.dof.d.scatter list background", kScatterHeader + (uint64_t)capacity * kScatterBytes, 0 });
        ID3D12PipelineState* quarterPso = fc.shaders.compute("Passes/Shading/DdofSetup.STEP1");
        g.addPass("m.dof.d.quarter", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(input, Use::SrvCompute);
                      b.use(quarter, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(input), 0, c.uav(quarter), 0, hw, hh, qw, qh };
                      c.cmd->SetPipelineState(quarterPso);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch((qw + 7) / 8, (qh + 7) / 8, 1);
                  });
        g.addPass("m.dof.d.scatter.clear", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(listForeground, Use::UavCompute);
                      b.use(listBackground, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(clear);
                      for (BufferRef list : { listForeground, listBackground })
                      {
                          const uint32_t k[4] = { c.uav(list), kScatterHeader / 4, 0, 0 };  // (the header: the record count)
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch(1, 1, 1);
                      }
                  });
    }
    const float neighbourMaxColour = (float)numberOr(fc, "shading.dof_diaphragm_scatter_neighbour_max_colour", 10.0);
    ID3D12PipelineState* reducePso = fc.shaders.compute("Passes/Shading/DdofReduce");
    g.addPass("m.dof.d.reduce", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(input, Use::SrvCompute);
                  for (uint32_t l = 0; l < levels; ++l) b.use(level[l], Use::UavCompute);
                  if (scatter)
                  {
                      b.use(quarter, Use::SrvCompute);
                      b.use(listForeground, Use::UavCompute);
                      b.use(listBackground, Use::UavCompute);
                  }
              },
              [=](PassContext& c) {
                  const uint32_t k[20] = { c.srv(input), scatter ? c.srv(quarter) : kNone, c.uav(level[0]), levels > 1 ? c.uav(level[1]) : kNone,
                                           levels > 2 ? c.uav(level[2]) : kNone, levels > 3 ? c.uav(level[3]) : kNone,
                                           scatter ? c.uav(listForeground) : kNone, scatter ? c.uav(listBackground) : kNone,
                                           hw, hh, levels, capacity,
                                           asUint(minScatterRadius), asUint(neighbourMaxColour),
                                           asUint(1.0f), asUint(squeeze),  // (the picture is exposed already: the exposure scale is 1)
                                           qw, qh, 0, 0 };
                  c.cmd->SetPipelineState(reducePso);
                  c.computeConstants(k, 20);
                  c.cmd->Dispatch(groupsX, groupsY, 1);
              });

    // the diaphragm's tables
    TextureRef edgeTable, gatherTable;
    if (shaped)
    {
        edgeTable = g.createTexture(TextureDesc{ "m.dof.d.bokeh edge", kLut, kLut, 1, 1, DXGI_FORMAT_R16_FLOAT });
        gatherTable = g.createTexture(TextureDesc{ "m.dof.d.bokeh gather", kLut, kLut, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
        ID3D12PipelineState* lutPso = fc.shaders.compute("Passes/Shading/DdofBokehLut");
        g.addPass("m.dof.d.bokeh", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(edgeTable, Use::UavCompute);
                      b.use(gatherTable, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[12] = { c.uav(edgeTable), c.uav(gatherTable), diaphragm.blades, 0,
                                               asUint(diaphragm.circumscribed), asUint(diaphragm.incircle), asUint(diaphragm.rotation), 0,
                                               asUint(diaphragm.bladeRadius), asUint(diaphragm.bladeOffset), 0, 0 };
                      c.cmd->SetPipelineState(lutPso);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch(kLut / 8, kLut / 8, 1);
                  });
    }

    // the gathers
    const TextureRef statistics = occlusion ? g.createTexture(TextureDesc{ "m.dof.d.background radii", hw, hh, 1, 1, DXGI_FORMAT_R16G16_FLOAT }) : TextureRef{};
    auto gather = [&](const char* name, const char* textureName, uint32_t layer) {
        const TextureRef out = g.createTexture(TextureDesc{ textureName, hw, hh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const bool withStatistics = layer == 2 && occlusion;
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/DdofGather.LAYER" + std::to_string(layer));
        g.addPass(name, QueueType::Graphics,
                  [&](PassBuilder& b) {
                      for (uint32_t l = 0; l < levels; ++l) b.use(level[l], Use::SrvCompute);
                      b.use(tiles.foreground, Use::SrvCompute);
                      b.use(tiles.background, Use::SrvCompute);
                      if (shaped)
                      {
                          b.use(edgeTable, Use::SrvCompute);
                          b.use(gatherTable, Use::SrvCompute);
                      }
                      b.use(out, Use::UavCompute);
                      if (withStatistics) b.use(statistics, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[24] = { c.srv(level[0]), c.srv(level[1]), c.srv(level[2]), c.srv(level[3]),
                                               c.srv(tiles.foreground), c.srv(tiles.background), c.uav(out), withStatistics ? c.uav(statistics) : kNone,
                                               hw, hh, pw, ph,
                                               rings, levels, shaped ? c.srv(gatherTable) : kNone, shaped ? c.srv(edgeTable) : kNone,
                                               asUint(squeeze), asUint(1.0f / squeeze), asUint(petzvalCorner), asUint(aspect),
                                               asUint(petzval[0]), asUint(petzval[1]), asUint(petzval[2]), asUint(petzval[3]) };
                      c.cmd->SetPipelineState(pso);
                      c.computeConstants(k, 24);
                      c.cmd->Dispatch(groupsX, groupsY, 1);
                  });
        return out;
    };
    TextureRef foreground = gather("m.dof.d.gather.foreground", "m.dof.d.foreground", 0);
    const TextureRef holeFilling = gather("m.dof.d.gather.hole filling", "m.dof.d.hole filling", 1);
    const TextureRef slight = gather("m.dof.d.gather.slight", "m.dof.d.slight", 3);
    TextureRef background = gather("m.dof.d.gather.background", "m.dof.d.background", 2);

    // the post filter of the two wide layers (shading.dof_diaphragm_postfilter: 0 none, 1 median, 2 largest)
    if (postfilter != 0)
    {
        ID3D12PipelineState* postfilterPso = fc.shaders.compute("Passes/Shading/DdofPostfilter");
        auto filter = [&](const char* name, const char* textureName, TextureRef from, uint32_t layer) {
            const TextureRef to = g.createTexture(TextureDesc{ textureName, hw, hh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            g.addPass(name, QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(from, Use::SrvCompute);
                          b.use(tiles.foreground, Use::SrvCompute);
                          b.use(tiles.background, Use::SrvCompute);
                          b.use(to, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(from), c.uav(to), c.srv(tiles.foreground), c.srv(tiles.background), hw, hh, layer, postfilter == 2 ? 2u : 1u };
                          c.cmd->SetPipelineState(postfilterPso);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch(groupsX, groupsY, 1);
                      });
            return to;
        };
        foreground = filter("m.dof.d.postfilter.foreground", "m.dof.d.foreground filtered", foreground, 0);
        background = filter("m.dof.d.postfilter.background", "m.dof.d.background filtered", background, 2);
    }

    // the sprites, added to the layer they belong to
    if (scatter)
    {
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shading/DdofScatter.ms";
        d.pixelShader = "Passes/Shading/DdofScatter.ps";
        d.renderTargets = { DXGI_FORMAT_R16G16B16A16_FLOAT };
        d.depthFormat = DXGI_FORMAT_UNKNOWN;
        d.depthWrite = false;
        d.cull = D3D12_CULL_MODE_NONE;
        d.premultipliedBlend = true;  // (the kernel's alpha is 0: rgb is added, the layer's alpha kept)
        ID3D12PipelineState* scatterPso = fc.shaders.mesh("m.dof.d.scatter", d);
        const uint32_t groups = (capacity + kSpritesPerGroup - 1) / kSpritesPerGroup;
        auto sprites = [&](const char* name, TextureRef layer, BufferRef list, bool isBackground) {
            const bool occluded = isBackground && occlusion;
            g.addPass(name, QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(layer, Use::RenderTarget);
                          b.use(list, Use::SrvGraphics);
                          if (shaped) b.use(edgeTable, Use::SrvGraphics);
                          if (occluded) b.use(statistics, Use::SrvGraphics);
                      },
                      [=](PassContext& c) {
                          const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(layer);
                          c.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                          D3D12_VIEWPORT vp{ 0, 0, (float)hw, (float)hh, 0, 1 };
                          D3D12_RECT sc{ 0, 0, (LONG)hw, (LONG)hh };
                          c.cmd->RSSetViewports(1, &vp);
                          c.cmd->RSSetScissorRects(1, &sc);
                          c.cmd->SetPipelineState(scatterPso);
                          // (the background's bokeh is the foreground's turned half a turn)
                          uint32_t k[36] = { c.srv(list), capacity, shaped ? c.srv(edgeTable) : kNone, occluded ? c.srv(statistics) : kNone,
                                             hw, hh, asUint(diaphragm.circumscribed), isBackground ? 1u : (uint32_t)-1,
                                             asUint(squeeze), asUint(barrelRadius), asUint(barrelLength), asUint(apertureRadius),
                                             asUint(tanHalf[0]), asUint(tanHalf[1]), asUint(focus), asUint(infinity),
                                             asUint(petzval[0]), asUint(petzval[1]), asUint(petzval[2]), asUint(petzval[3]),
                                             asUint(petzvalCorner), asUint(aspect), 0, 0 };
                          std::memcpy(&k[24], flags, 48);
                          c.graphicsConstants(k, 36);
                          c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                      });
        };
        sprites("m.dof.d.scatter.foreground", foreground, listForeground, false);
        sprites("m.dof.d.scatter.background", background, listBackground, true);
    }

    // the recombine
    const float jitterX = prefilter ? fc.frame.upscale.jitterX : 0.0f, jitterY = prefilter ? fc.frame.upscale.jitterY : 0.0f;
    const float widen = (float)numberOr(fc, "shading.dof_diaphragm_stable_range_boost", 0.0);
    const uint32_t frame = (uint32_t)fc.frame.frameIndex;
    ID3D12PipelineState* recombinePso = fc.shaders.compute("Passes/Shading/DdofRecombine");
    g.addPass("m.dof.d.recombine", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  for (TextureRef t : { foreground, holeFilling, slight, background }) b.use(t, Use::SrvCompute);
                  if (shaped) b.use(edgeTable, Use::SrvCompute);
                  b.use(dst, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[24] = { c.srv(src), c.srv(depth), c.uav(dst), shaped ? c.srv(edgeTable) : kNone,
                                     c.srv(foreground), c.srv(holeFilling), c.srv(slight), c.srv(background),
                                     w, h, hw, hh };
                  std::memcpy(&k[12], lens, 16);
                  k[16] = asUint(jitterX);
                  k[17] = asUint(jitterY);
                  k[18] = frame;
                  k[19] = pairs;
                  k[20] = asUint(widen);
                  k[21] = asUint(depthBlur[0]);
                  k[22] = asUint(depthBlur[1]);
                  k[23] = asUint(squeeze);
                  c.cmd->SetPipelineState(recombinePso);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    return stats;
}
} // namespace unx::render::shading
