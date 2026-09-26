#pragma once
// Driver of VsmBlockCheck.hlsl (VsmTests, LocalShadowTests): after tracks::shadowPages in a tf.run frame, checks the
// block hierarchy of every page rendered this frame against its atlas texels by brute force. vsmBlockCheck records the
// pass and returns the readback; vsmBlockCheckReport decodes it after the run and reports each count.
#include "TestRaster.h"
#include "VsmSystem.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace unx::stest
{
struct VsmBlockCheckResult
{
    uint32_t pages, rangeBad, residualBad, refBad;
    float worst;
    uint32_t texels, width, nonEmpty;
};

inline std::shared_ptr<std::vector<uint8_t>> vsmBlockCheck(TestFrame& tf, FramePassContext& fc)
{
    const uint32_t zero[8] = {};
    const BufferRef o = tf.uploadBuffer(fc, zero, sizeof(zero), 0, "block check out");
    const FrameResources r = fc.resources;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/VsmBlockCheck");
    fc.graph.addPass("s.test.blockcheck", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.vsmPageTable, Use::SrvCompute);
                         b.use(r.vsmAtlas, Use::SrvCompute);
                         b.use(r.vsmBlocks, Use::SrvCompute);
                         b.use(o, Use::UavCompute);
                     },
                     [=](PassContext& ctx) {
                         const uint32_t slots = shadow::kSlots;
                         const uint32_t k[8] = { ctx.srv(r.vsmPageTable), ctx.srv(r.vsmAtlas), ctx.srv(r.vsmBlocks), r.vsmConstants,
                                                 r.vsmLocalLights, ctx.uav(o), slots, 0 };
                         ctx.cmd->SetPipelineState(pso);
                         ctx.computeConstants(k, 8);
                         ctx.cmd->Dispatch(std::min(slots, 65535u), (slots + 65534) / 65535, 1);
                     });
    return tf.readbackBuffer(fc, o, sizeof(zero));
}

// report(ok, what, value, limit) of the calling test.
inline VsmBlockCheckResult vsmBlockCheckReport(const std::vector<uint8_t>& data, const char* label,
                                               const std::function<void(bool, const char*, double, double)>& report)
{
    VsmBlockCheckResult c;
    std::memcpy(&c, data.data(), sizeof(c));
    logf("%s: block hierarchy of %u pages (%u non-empty 8-blocks, %u texels): range mismatches %u, residual violations %u "
         "(largest %.3g m), ref mismatches %u, mean 8-block residual width %.4g mm\n",
         label, c.pages, c.nonEmpty, c.texels, c.rangeBad, c.residualBad, c.worst, c.refBad,
         c.nonEmpty ? c.width * 1e-2 / c.nonEmpty : 0.0);
    report(c.pages > 0 && c.texels > 0, "VSM block check: pages and caster texels checked", c.pages, 1);
    report(c.rangeBad == 0, "VSM block check: block key ranges exact (every level)", c.rangeBad, 0);
    report(c.residualBad == 0, "VSM block check: every texel inside its blocks' plane residual bounds", c.residualBad, 0);
    report(c.refBad == 0, "VSM block check: page reference = highest caster", c.refBad, 0);
    return c;
}
} // namespace unx::stest
