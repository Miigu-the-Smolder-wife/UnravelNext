// Depth of field of M (render A item A5; FEATURES_GAME 4.1, COVERAGE 14.12 (2c)): the thin lens's aperture integral over
// the pinhole image (after the shutter's time integral, before the post chain). The lens is FrameContext::lensAperture
// (diameter, m) and lensFocus (m along the view axis), the reference tracer's photo-mode camera. A pixel's signed
// circle-of-confusion radius is rho = f_px A (1/z_f - 1/z) / 2 px (f_px = (H/2) proj[1][1]); the passes:
//   DofSetup  rho per pixel, per 32 px tile and radius octave the largest |rho|, the octave pyramid (octave c's pixels
//             over their level-c texel: colour, area share, luminance centroid and spread; c = 1..5, tile sums for 6, 7);
//   DofDown   octaves 6 and 7 from the tile sums;
//   DofReach  per tile and octave, the largest radius of the sources whose disk reaches the tile;
//   DofGather per pixel, every reaching octave's sources at their own level (DofGather.hlsl has the composite).
#include "unx/shading/DepthOfField.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstring>

namespace unx::render::shading
{
namespace
{
constexpr uint32_t kTile = 32, kOctaves = 8;
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

bool depthOfFieldActive(FramePassContext& fc, const ViewResources& view)
{
    return view.view.kind == gpu::ViewKind::Main && fc.frame.lensAperture > 0 && fc.frame.lensFocus > 0 && view.depth.valid();
}

BufferRef depthOfField(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, DepthOfFieldProducts* products)
{
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height, tilesX = (w + kTile - 1) / kTile, tilesY = (h + kTile - 1) / kTile;
    const uint64_t tiles = (uint64_t)tilesX * tilesY;
    const float fpx = 0.5f * (float)h * view.view.proj.m[1][1];
    const float k0 = 0.5f * fpx * fc.frame.lensAperture / fc.frame.lensFocus, k1 = 0.5f * fpx * fc.frame.lensAperture / view.view.nearPlane;
    const TextureRef depth = view.depth;
    const TextureRef coc = g.createTexture(TextureDesc{ "m.dof.coc", w, h, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const BufferRef maxima = g.createBuffer({ "m.dof.tile maxima", tiles * kOctaves * 4, 0 });
    const BufferRef reach = g.createBuffer({ "m.dof.reach", tiles * kOctaves * 4, 0 });
    const BufferRef sums = g.createBuffer({ "m.dof.tile sums", tiles * 80, 0 });
    const BufferRef stats = g.createBuffer({ "m.dof.stats", 40, 0 });  // pixels gathered, radii clamped, 8 octave maxima
    TextureRef A[kOctaves], S[kOctaves];  // [1..7]: colour and area share, shape (DofCommon.hlsli)
    for (uint32_t c = 1; c < kOctaves; ++c)
    {
        const uint32_t lw = (w + (1u << c) - 1) >> c, lh = (h + (1u << c) - 1) >> c;
        A[c] = g.createTexture(TextureDesc{ "m.dof.level colour", lw, lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        S[c] = g.createTexture(TextureDesc{ "m.dof.level shape", lw, lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    }
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");  // (zeroes a raw buffer)
    ID3D12PipelineState* setup = fc.shaders.compute("Passes/Shading/DofSetup");
    ID3D12PipelineState* down = fc.shaders.compute("Passes/Shading/DofDown");
    ID3D12PipelineState* reachPso = fc.shaders.compute("Passes/Shading/DofReach");
    ID3D12PipelineState* gather = fc.shaders.compute("Passes/Shading/DofGather");
    g.addPass("m.dof.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(stats), 10, 0, 0 };
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("m.dof.setup", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(src, Use::SrvCompute);
                  b.use(coc, Use::UavCompute);
                  b.use(maxima, Use::UavCompute);
                  b.use(stats, Use::UavCompute);
                  b.use(sums, Use::UavCompute);
                  for (uint32_t c = 1; c <= 5; ++c)
                  {
                      b.use(A[c], Use::UavCompute);
                      b.use(S[c], Use::UavCompute);
                  }
              },
              [=](PassContext& c) {
                  const uint32_t k[24] = { c.srv(depth), c.srv(src), c.uav(coc), c.uav(maxima),
                                           w, h, tilesX, c.uav(stats),
                                           asUint(k0), asUint(k1), c.uav(sums), 0,
                                           c.uav(A[1]), c.uav(A[2]), c.uav(A[3]), c.uav(A[4]),
                                           c.uav(A[5]), c.uav(S[1]), c.uav(S[2]), c.uav(S[3]),
                                           c.uav(S[4]), c.uav(S[5]), 0, 0 };
                  c.cmd->SetPipelineState(setup);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    const uint32_t w6 = (w + 63) / 64, h6 = (h + 63) / 64;
    g.addPass("m.dof.down", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(sums, Use::SrvCompute);
                  for (uint32_t c = 6; c <= 7; ++c)
                  {
                      b.use(A[c], Use::UavCompute);
                      b.use(S[c], Use::UavCompute);
                  }
              },
              [=](PassContext& c) {
                  const uint32_t k[12] = { c.srv(sums), c.uav(A[6]), c.uav(S[6]), c.uav(A[7]), c.uav(S[7]), tilesX, tilesY, 0, w, h, 0, 0 };
                  c.cmd->SetPipelineState(down);
                  c.computeConstants(k, 12);
                  c.cmd->Dispatch((w6 + 7) / 8, (h6 + 7) / 8, 1);
              });
    g.addPass("m.dof.reach", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(maxima, Use::SrvCompute);
                  b.use(stats, Use::SrvCompute);
                  b.use(reach, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(maxima), c.uav(reach), tilesX, tilesY, c.srv(stats), 0, 0, 0 };
                  c.cmd->SetPipelineState(reachPso);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
              });
    g.addPass("m.dof.gather", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(coc, Use::SrvCompute);
                  b.use(reach, Use::SrvCompute);
                  b.use(dst, Use::UavCompute);
                  b.use(stats, Use::UavCompute);
                  for (uint32_t c = 1; c < kOctaves; ++c)
                  {
                      b.use(A[c], Use::SrvCompute);
                      b.use(S[c], Use::SrvCompute);
                  }
              },
              [=](PassContext& c) {
                  const uint32_t k[24] = { c.srv(src), c.srv(coc), c.srv(reach), c.uav(dst),
                                           w, h, tilesX, c.uav(stats),
                                           c.srv(A[1]), c.srv(A[2]), c.srv(A[3]), c.srv(A[4]),
                                           c.srv(A[5]), c.srv(A[6]), c.srv(A[7]), c.srv(S[1]),
                                           c.srv(S[2]), c.srv(S[3]), c.srv(S[4]), c.srv(S[5]),
                                           c.srv(S[6]), c.srv(S[7]), 0, 0 };
                  c.cmd->SetPipelineState(gather);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    if (products)
    {
        products->coc = coc;
        products->maxima = maxima;
        products->reach = reach;
        for (uint32_t c = 1; c < kOctaves; ++c) products->colour[c] = A[c], products->shape[c] = S[c];
    }
    return stats;
}
} // namespace unx::render::shading
