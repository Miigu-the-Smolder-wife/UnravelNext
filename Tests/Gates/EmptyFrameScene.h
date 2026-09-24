#pragma once
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

namespace unx::test
{
using namespace unx::render;

// P0b empty-frame gate (ARCHITECTURE_KO.md 7.1-3): the frame graph of 4.1 with every pass reduced to one thread
// group (or one tiny triangle) that touches each resource it declares. Resources are real: 4K/1440p transients that
// alias one heap, a persistent VSM depth pool, persistent GI/particle/TLAS buffers and an RGB10A2 output. What
// remains is the graph's own cost: barriers and layout transitions, discards of aliased memory, cross-queue fences,
// per-pass timestamps, dispatch/draw fixed cost and CPU declaration/record/submit.
class EmptyFrameScene
{
public:
    static constexpr uint32_t kPassCount = 120;

    EmptyFrameScene(Device& device, ShaderLibrary& shaders, uint32_t width, uint32_t height);
    // passes = 0 declares only the output pass (frame markers + one pass) for the baseline.
    void build(RenderGraph& graph, uint64_t frame, bool fullGraph = true);
    ID3D12Resource* output() const { return m_output.Get(); }

private:
    Device& m_device;
    ShaderLibrary& m_shaders;
    uint32_t m_width, m_height;
    ComPtr<ID3D12Resource> m_output, m_vsmPool, m_giCache, m_particleState, m_tlas;
};
} // namespace unx::test
