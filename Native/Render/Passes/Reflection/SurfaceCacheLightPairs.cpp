// surface_cache.direct_pairs (Passes/SurfaceCache/SurfaceCacheLightPairs.hlsli; owner A for S2's surface cache): the
// cells' direct light in three passes - select (compute, no ray), trace (one shadow ray per pair record, bands of at most
// kThreadsPerDispatch threads), store (compute). Transient buffers: the frame's cells (4 B a cell) and pairs (9 x 32 B).
#include "unx/refl/SurfaceCacheLightPairs.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cstring>

namespace unx::render::refl
{
namespace
{
constexpr uint32_t kPairs = 9, kPairBytes = 32;  // SurfaceCacheLightPairs.hlsli SCP_PAIRS, SCP_PAIR_BYTES
// The structural bound of one dispatch (Docs/Status/DISPATCH_BOUNDS_KO.md): a thread traces at most one ray.
constexpr uint32_t kThreadsPerDispatch = 262144;
const char* const kSelect[2] = { "Passes/SurfaceCache/SurfaceCacheLightPairsSelect.SKY0", "Passes/SurfaceCache/SurfaceCacheLightPairsSelect.SKY1" };

uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

void recordSurfaceCacheLightPairs(FramePassContext& fc, const SurfaceCachePairsInputs& in)
{
    if (!in.surfaceCache.valid() || in.budget == 0) return;
    const QualityConfig& q = fc.quality;
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const bool inlineQuery = q.has("surface_cache.direct_pairs_inline") && q.boolean("surface_cache.direct_pairs_inline");
    // the lights' default ray end bias: MegaLights' (a light's own value - scene::Light::rayEndBias - comes first)
    const float endBias = q.has("shading.mega_lights_ray_end_bias_m") ? (float)q.number("shading.mega_lights_ray_end_bias_m") : 0.01f;
    const uint32_t budget = in.budget, frame = in.frame, lightFlags = in.lightFlags;
    const uint64_t pairCount = (uint64_t)budget * kPairs;
    if ((budget + 63) / 64 > 65535) fail("surface_cache.direct_pairs: %u cells a frame exceed one dispatch row of the select pass", budget);
    const BufferRef cache = in.surfaceCache;
    const BufferRef cells = g.createBuffer({ "r.sc.pairs cells", (uint64_t)budget * 4, 0 });
    const BufferRef pairs = g.createBuffer({ "r.sc.pairs", pairCount * kPairBytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS cb = in.frameConstants;
    const auto declareShared = in.declareShared;
    const auto sharedConstants = in.sharedConstants;
    auto constants = [=](PassContext& c, uint32_t k[32], uint32_t cellsIndex, uint32_t pairsIndex, uint32_t first) {
        sharedConstants(c, k);
        k[0] = c.uav(cache);
        k[1] = budget;
        k[2] = frame;
        k[3] = lightFlags;
        k[16] = cellsIndex;
        k[17] = pairsIndex;
        k[18] = first;
        k[19] = bits(endBias);
    };

    ID3D12PipelineState* select = shaders.compute(kSelect[in.skyVariant & 1u]);
    g.addPass("r.sc.pairs.select", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavGraphics);
                  declareShared(b);
                  b.use(cells, Use::UavCompute);
                  b.use(pairs, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[32] = {};
                  constants(c, k, c.uav(cells), c.uav(pairs), 0);
                  c.cmd->SetPipelineState(select);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 32);
                  c.cmd->Dispatch((budget + 63) / 64, 1, 1);
              });

    if (inlineQuery)
    {
        ID3D12PipelineState* trace = shaders.compute("Passes/SurfaceCache/SurfaceCacheLightPairsTraceInline");
        g.addPass("r.sc.pairs.trace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(cache, Use::UavGraphics);
                      declareShared(b);
                      b.use(cells, Use::SrvCompute);
                      b.use(pairs, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[32] = {};
                      c.cmd->SetPipelineState(trace);
                      c.bindFrameConstants(cb);
                      for (uint64_t first = 0; first < pairCount; first += kThreadsPerDispatch)
                      {
                          constants(c, k, c.srv(cells), c.uav(pairs), (uint32_t)first);
                          c.computeConstants(k, 32);
                          c.cmd->Dispatch((uint32_t)((std::min<uint64_t>(kThreadsPerDispatch, pairCount - first) + 63) / 64), 1, 1);
                      }
                  });
    }
    else
    {
        rt::RayPipeline& pipeline =
            rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline("Passes/SurfaceCache/SurfaceCacheLightPairsTrace", { "SurfaceCachePairsTraceGen" }));
        g.addPass("r.sc.pairs.trace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(cache, Use::UavGraphics);
                      declareShared(b);
                      b.use(cells, Use::SrvGraphics);
                      b.use(pairs, Use::UavGraphics);
                  },
                  [=, &pipeline](PassContext& c) {
                      uint32_t k[32] = {};
                      c.bindFrameConstants(cb);
                      for (uint64_t first = 0; first < pairCount; first += kThreadsPerDispatch)
                      {
                          constants(c, k, c.srv(cells), c.uav(pairs), (uint32_t)first);
                          c.computeConstants(k, 32);
                          pipeline.dispatch(c.cmd, 0, (uint32_t)std::min<uint64_t>(kThreadsPerDispatch, pairCount - first), 1);
                      }
                  });
    }

    ID3D12PipelineState* store = shaders.compute("Passes/SurfaceCache/SurfaceCacheLightPairsStore");
    g.addPass("r.sc.pairs.store", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavGraphics);
                  declareShared(b);
                  b.use(cells, Use::SrvCompute);
                  b.use(pairs, Use::SrvCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[32] = {};
                  constants(c, k, c.srv(cells), c.srv(pairs), 0);
                  c.cmd->SetPipelineState(store);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 32);
                  c.cmd->Dispatch((budget + 63) / 64, 1, 1);
              });
}
} // namespace unx::render::refl
