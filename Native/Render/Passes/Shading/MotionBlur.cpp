// Motion blur of M (render A item A5; COVERAGE 14.12 (2b)): the exposure integral over the shutter [t - s dt, t] by a
// depth-aware gather along the screen velocity (MotionVelocity.hlsl -> MotionTiles.hlsl -> MotionBlur.hlsl). The shutter s
// is a fraction of the frame interval (shading.motion_blur_shutter; 0.5 = 180 degrees, the user's gate camera). Runs only
// in frames with motion: a static frame's image is already the integral.
#include "unx/shading/MotionBlur.h"
#include "unx/core/Config.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cstring>

namespace unx::render::shading
{
namespace
{
constexpr uint32_t kTile = 32;  // MOTION_TILE
float shutterOf(const QualityConfig& q)
{
    const float s = q.has("shading.motion_blur_shutter") ? (float)q.number("shading.motion_blur_shutter") : 0.0f;
    if (!(s >= 0 && s <= 1)) fail("shading.motion_blur_shutter %g: the shutter as a fraction of the frame interval in [0, 1]", s);
    return s;
}
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

bool motionBlurActive(FramePassContext& fc, const ViewResources& view)
{
    if (view.view.kind != gpu::ViewKind::Main || !(shutterOf(fc.quality) > 0)) return false;
    if (!view.visId.valid() || !view.visibleClusters.valid() || !view.depth.valid()) return false;
    const bool cameraMoved = std::memcmp(&view.view.viewProj, &view.view.prevViewProj, sizeof(float4x4)) != 0;
    return cameraMoved || fc.scene.hasMotion();
}

TextureRef motionVelocity(FramePassContext& fc, const ViewResources& view)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const TextureRef velocity = g.createTexture(TextureDesc{ "m.motion.velocity", w, h, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/MotionVelocity");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef vis = view.visId;
    const BufferRef clusters = view.visibleClusters;
    g.addPass("m.motion.velocity", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(vis, Use::SrvCompute);
                  b.use(clusters, Use::SrvCompute);
                  b.use(velocity, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(vis), c.srv(clusters), c.uav(velocity), 0, w, h, 0, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    return velocity;
}

void motionBlur(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst)
{
    motionBlurWithVelocity(fc, view, src, dst, motionVelocity(fc, view));
}

void motionBlurWithVelocity(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, TextureRef velocity)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const uint32_t tw = (w + kTile - 1) / kTile, th = (h + kTile - 1) / kTile;
    const float shutter = shutterOf(fc.quality);
    const TextureRef tiles = g.createTexture(TextureDesc{ "m.motion.tiles", tw, th, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    const TextureRef neighbour = g.createTexture(TextureDesc{ "m.motion.neighbour", tw, th, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    ID3D12PipelineState* tileMax = fc.shaders.compute("Passes/Shading/MotionTiles.STEP0");
    ID3D12PipelineState* neighbourMax = fc.shaders.compute("Passes/Shading/MotionTiles.STEP1");
    ID3D12PipelineState* gather = fc.shaders.compute("Passes/Shading/MotionBlur");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    g.addPass("m.motion.tiles", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(velocity, Use::SrvCompute);
                  b.use(tiles, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(velocity), c.uav(tiles), w, h };
                  c.cmd->SetPipelineState(tileMax);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(tw, th, 1);
              });
    g.addPass("m.motion.neighbour", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(tiles, Use::SrvCompute);
                  b.use(neighbour, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(tiles), c.uav(neighbour), tw, th };
                  c.cmd->SetPipelineState(neighbourMax);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);
              });
    const TextureRef depth = view.depth;
    const uint32_t frame = (uint32_t)fc.frame.frameIndex;
    g.addPass("m.motion.blur", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(velocity, Use::SrvCompute);
                  b.use(neighbour, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(dst, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[12] = { c.srv(src), c.srv(velocity), c.srv(neighbour), c.srv(depth), c.uav(dst), w, h, frame, asUint(shutter), 0, 0, 0 };
                  c.cmd->SetPipelineState(gather);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 12);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
}
bool distortionActive(FramePassContext&, const ViewResources& view)
{
    return view.view.kind == gpu::ViewKind::Main && view.distortionOffset.valid() && view.distortionDepth.valid() && view.depth.valid();
}

void distortion(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Distortion");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef offset = view.distortionOffset, haze = view.distortionDepth, depth = view.depth;
    g.addPass("m.distortion", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(offset, Use::SrvCompute);
                  b.use(haze, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(dst, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(src), c.uav(dst), c.srv(offset), c.srv(haze), c.srv(depth), w, h, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
}
} // namespace unx::render::shading
