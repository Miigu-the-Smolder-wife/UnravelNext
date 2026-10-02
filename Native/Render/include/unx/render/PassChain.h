#pragma once
// A run of small passes timed as one (UE6_WORKPLAN_KO.md 8.1 item 6, 8.3). The steps are ordinary graph passes, added
// in order with their own declared uses; folding joins them into one profiler scope (RenderGraph::joinPasses): one
// timestamp and one marker for the run. The barriers between the steps are the plan's, so a step may read as a shader
// resource, as indirect arguments or as a copy source what the step before wrote as a UAV, and steps that touch
// different resources get no barrier between them. With 'fold' false every step is a scope under its own name (the A/B
// path: the tracks' *.fold_small_passes switches).
// What a fold is worth [measured, ARCHITECTURE_KO.md 1.2 item 7]: the timestamp of a pass boundary, 0.11 us. The fixed
// cost of a dispatch that depends on the one before (0.85 us: the barrier that orders them) stays with the barrier.
#include "unx/render/RenderGraph.h"

#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
class PassChain
{
public:
    PassChain(RenderGraph& graph, QueueType queue, bool fold) : m_graph(graph), m_queue(queue), m_fold(fold) {}

    // A step: a pass's setup and execute as for RenderGraph::addPass (the setup runs here).
    void add(const std::string& name, const RenderGraph::SetupFn& setup, RenderGraph::ExecuteFn execute)
    {
        if (m_fold) m_steps.push_back(m_graph.passCount());
        m_graph.addPass(name, m_queue, setup, std::move(execute));
    }

    // The steps added since the last flush as one scope 'name' (steps another pass was added between: a scope each side
    // of it, under the same name). Nothing to do without steps, or when not folding.
    void flush(const std::string& name)
    {
        for (size_t i = 0; i < m_steps.size();)
        {
            size_t end = i + 1;
            while (end < m_steps.size() && m_steps[end] == m_steps[end - 1] + 1) ++end;
            m_graph.joinPasses(m_steps[i], (uint32_t)(end - i), name);
            i = end;
        }
        m_steps.clear();
    }

private:
    RenderGraph& m_graph;
    QueueType m_queue;
    bool m_fold;
    std::vector<uint32_t> m_steps;  // graph pass indices of the steps since the last flush
};
} // namespace unx::render
