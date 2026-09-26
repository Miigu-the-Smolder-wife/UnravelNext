// Motion blur of M (render A item A5; COVERAGE 14.12 (2)): the exposure integral over the shutter [t - s dt, t]. The shutter
// s is a fraction of the frame interval (shading.motion_blur_shutter; 0.5 = 180 degrees, the user's gate camera). Runs
// only in frames with motion: a static frame's image is already the integral.
//   (2b) a depth-aware gather along the screen velocity (MotionVelocity.hlsl -> MotionTiles.hlsl -> MotionBlur.hlsl),
//        streaks up to 32 px;
//   (2a) the camera's rotation, exact at every depth, over the rotation axis's latitude circles (MotionRotation.hlsl),
//        when its streak at the image centre exceeds 16 px and the axis is outside the view (yaw and pitch: the
//        first-person turn); the gather then takes the residual velocity (translation, objects). Roll about an axis in the
//        view stays with the gather (its streaks are short near the axis).
// And the heat haze composite of E's fields (Distortion.hlsl).
#include "unx/shading/MotionBlur.h"
#include "unx/core/Config.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
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

// The rotation stage's parameters of a frame (inactive: the gather takes the whole velocity).
struct RotationStage
{
    bool active = false;
    float3 qt[3] = {};         // rows of Q^T (view space): the previous direction of what a pixel sees now
    float3 a, e1, e2;          // axis and the basis of its latitude circles (e1 = the forward projected off the axis)
    float arc = 0;             // s phi (radians)
    float lambda0 = 0, beta0 = 0, texel = 0;
    uint32_t mapWidth = 0, mapHeight = 0;
};

float3 row3(const float4x4& m, int r) { return float3{ m.m[r][0], m.m[r][1], m.m[r][2] }; }

// q: the rows of Q = R_cur R_prev^T (view space: the current direction of what the previous view saw along a direction).
RotationStage rotationStage(const ViewDesc& v, float shutter, const float3 q[3])
{
    RotationStage r;
    const float trace = q[0].x + q[1].y + q[2].z;
    const float phi = std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f));
    if (!(phi > 1e-6f)) return r;
    const float3 axis{ q[2].y - q[1].z, q[0].z - q[2].x, q[1].x - q[0].y };
    const float len = std::sqrt(dot(axis, axis));
    if (!(len > 1e-9f)) return r;  // phi near pi: not a frame's turn
    r.a = axis * (1.0f / len);
    r.arc = shutter * phi;
    const float pxPerRad = 0.5f * (float)v.height * v.proj.m[1][1];  // at the image centre
    if (r.arc * pxPerRad <= 16.0f) return r;                          // the gather covers it
    // the axis outside the view, with a margin beyond the half-diagonal field of view
    const float tanV = 1.0f / v.proj.m[1][1], tanH = 1.0f / v.proj.m[0][0];
    const float halfDiagonal = std::atan(std::sqrt(tanV * tanV + tanH * tanH));
    const float3 forward{ 0, 0, -1 };
    if (!(std::acos(std::clamp(std::abs(dot(r.a, forward)), 0.0f, 1.0f)) > halfDiagonal + 0.05f)) return r;
    r.e1 = normalize(forward - r.a * dot(forward, r.a));
    r.e2 = cross(r.a, r.e1);
    for (int i = 0; i < 3; ++i) r.qt[i] = float3{ (&q[0].x)[i], (&q[1].x)[i], (&q[2].x)[i] };
    // the screen's (lambda, beta) extent from its border, then the arc beyond it
    float lmin = 1e9f, lmax = -1e9f, bmin = 1e9f, bmax = -1e9f;
    const int steps = 64;
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float)i / steps;
            const float nx = e < 2 ? 2 * t - 1 : (e == 2 ? -1.0f : 1.0f), ny = e >= 2 ? 2 * t - 1 : (e == 0 ? -1.0f : 1.0f);
            const float3 d = normalize(float3{ nx * tanH, ny * tanV, -1.0f });
            const float lambda = std::atan2(dot(d, r.e2), dot(d, r.e1)), beta = std::asin(std::clamp(dot(d, r.a), -1.0f, 1.0f));
            lmin = std::min(lmin, lambda);
            lmax = std::max(lmax, lambda);
            bmin = std::min(bmin, beta);
            bmax = std::max(bmax, beta);
        }
    r.texel = 2.0f / (v.proj.m[1][1] * (float)v.height);  // the centre pixel's angle
    const float margin = 2 * r.texel;
    r.lambda0 = lmin - margin;
    r.beta0 = bmin - margin;
    r.mapWidth = (uint32_t)std::ceil((lmax + r.arc + margin - r.lambda0) / r.texel) + 1;
    r.mapHeight = (uint32_t)std::ceil((bmax + margin - r.beta0) / r.texel) + 1;
    if ((uint64_t)r.mapWidth * r.mapHeight > 6ull * v.width * v.height) return RotationStage{};  // a turn beyond a frame's reach
    r.active = true;
    return r;
}

// The frame's camera rotation from its view and the previous view-projection (the same projection; a changed field of
// view is not a rotation: no rotation stage).
RotationStage frameRotation(const ViewDesc& v, float shutter)
{
    const float4x4 prevView = mul(inverse(v.proj), v.prevViewProj);
    float3 rp[3], rc[3], q[3];
    for (int i = 0; i < 3; ++i)
    {
        rp[i] = row3(prevView, i);
        rc[i] = row3(v.view, i);
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(dot(rp[i], rp[j]) - (i == j ? 1.0f : 0.0f)) > 1e-3f) return RotationStage{};
    for (int i = 0; i < 3; ++i) q[i] = float3{ dot(rc[i], rp[0]), dot(rc[i], rp[1]), dot(rc[i], rp[2]) };  // (R_cur R_prev^T)_ij
    return rotationStage(v, shutter, q);
}

TextureRef velocityPass(FramePassContext& fc, const ViewResources& view, const RotationStage* rotation)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const TextureRef velocity = g.createTexture(TextureDesc{ "m.motion.velocity", w, h, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/MotionVelocity");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef vis = view.visId;
    const BufferRef clusters = view.visibleClusters;
    const RotationStage r = rotation ? *rotation : RotationStage{};
    g.addPass("m.motion.velocity", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(vis, Use::SrvCompute);
                  b.use(clusters, Use::SrvCompute);
                  b.use(velocity, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[20] = { c.srv(vis), c.srv(clusters), c.uav(velocity), 0, w, h, r.active ? 1u : 0u, 0 };
                  for (int i = 0; i < 3; ++i)
                  {
                      k[8 + 4 * i] = asUint(r.qt[i].x);
                      k[9 + 4 * i] = asUint(r.qt[i].y);
                      k[10 + 4 * i] = asUint(r.qt[i].z);
                      k[11 + 4 * i] = 0;
                  }
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 20);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    return velocity;
}

void gatherPasses(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, TextureRef velocity)
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

void rotationPasses(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, const RotationStage& r)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const TextureRef map = g.createTexture(TextureDesc{ "m.motion.rotationMap", r.mapWidth, r.mapHeight, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    ID3D12PipelineState* fill = fc.shaders.compute("Passes/Shading/MotionRotation.STEP0");
    ID3D12PipelineState* scan = fc.shaders.compute("Passes/Shading/MotionRotation.STEP1");
    ID3D12PipelineState* read = fc.shaders.compute("Passes/Shading/MotionRotation.STEP2");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    // A12: view-model pixels stay out of the rotation (their texels weigh 0 in the map, their pixels keep their value); the
    // vis buffer is read only in frames with a view model
    const bool viewModels = fc.scene.viewModelInstances() > 0;
    const TextureRef visId = view.visId;
    const BufferRef clusters = view.visibleClusters;
    auto constants = [=](uint32_t (&k)[24], uint32_t image, uint32_t mapIndex, uint32_t out, uint32_t vis, uint32_t visible) {
        const uint32_t v[24] = { image, mapIndex, out, vis, r.mapWidth, r.mapHeight, w, h, asUint(r.lambda0), asUint(r.beta0), asUint(r.texel), asUint(r.arc),
                                 asUint(r.a.x), asUint(r.a.y), asUint(r.a.z), visible, asUint(r.e1.x), asUint(r.e1.y), asUint(r.e1.z), 0,
                                 asUint(r.e2.x), asUint(r.e2.y), asUint(r.e2.z), 0 };
        std::memcpy(k, v, sizeof v);
    };
    g.addPass("m.motion.rotation.map", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(map, Use::UavCompute);
                  if (viewModels)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
              },
              [=](PassContext& c) {
                  uint32_t k[24];
                  constants(k, c.srv(src), c.uav(map), 0, viewModels ? c.srv(visId) : gpu::kNone, viewModels ? c.srv(clusters) : gpu::kNone);
                  c.cmd->SetPipelineState(fill);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((r.mapWidth + 7) / 8, (r.mapHeight + 7) / 8, 1);
              });
    g.addPass("m.motion.rotation.scan", QueueType::Graphics, [&](PassBuilder& b) { b.use(map, Use::UavCompute); },
              [=](PassContext& c) {
                  uint32_t k[24];
                  constants(k, 0, c.uav(map), 0, gpu::kNone, gpu::kNone);
                  c.cmd->SetPipelineState(scan);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch(r.mapHeight, 1, 1);
              });
    g.addPass("m.motion.rotation", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(map, Use::SrvCompute);
                  b.use(dst, Use::UavCompute);
                  if (viewModels)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
              },
              [=](PassContext& c) {
                  uint32_t k[24];
                  constants(k, c.srv(src), c.srv(map), c.uav(dst), viewModels ? c.srv(visId) : gpu::kNone, viewModels ? c.srv(clusters) : gpu::kNone);
                  c.cmd->SetPipelineState(read);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
}
} // namespace

bool motionBlurActive(FramePassContext& fc, const ViewResources& view)
{
    if (view.view.kind != gpu::ViewKind::Main || !(shutterOf(fc.quality) > 0)) return false;
    if (!view.visId.valid() || !view.visibleClusters.valid() || !view.depth.valid()) return false;
    const bool cameraMoved = std::memcmp(&view.view.viewProj, &view.view.prevViewProj, sizeof(float4x4)) != 0;
    return cameraMoved || fc.scene.hasMotion();
}

TextureRef motionVelocity(FramePassContext& fc, const ViewResources& view) { return velocityPass(fc, view, nullptr); }

void motionBlur(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst)
{
    const RotationStage rotation = frameRotation(view.view, shutterOf(fc.quality));
    const TextureRef velocity = velocityPass(fc, view, rotation.active ? &rotation : nullptr);
    if (!rotation.active)
    {
        gatherPasses(fc, view, src, dst, velocity);
        return;
    }
    // the residual (translation, objects) first, then the rotation's arc
    const TextureRef residual = fc.graph.createTexture(TextureDesc{ "m.motion.residual", view.view.width, view.view.height, 1, 1, fc.graph.desc(dst).format });
    gatherPasses(fc, view, src, residual, velocity);
    rotationPasses(fc, view, residual, dst, rotation);
}

void motionBlurWithVelocity(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, TextureRef velocity)
{
    gatherPasses(fc, view, src, dst, velocity);
}

bool motionRotationBlur(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, const float3 (&q)[3])
{
    const RotationStage rotation = rotationStage(view.view, shutterOf(fc.quality), q);
    if (rotation.active) rotationPasses(fc, view, src, dst, rotation);
    return rotation.active;
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
