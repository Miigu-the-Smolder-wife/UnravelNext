#pragma once
// Several small dispatches as one graph pass (UE6_WORKPLAN_KO.md 8.1 item 6). Passes that follow each other, are a group
// or a few each and use their resources in ways one pass may declare together (a buffer one of them writes as a UAV is
// a UAV for all; reads combine) cost a barrier batch and a timestamp pair each as passes of their own. As steps of one
// pass they run in the same order, each after the first behind a global UAV barrier, so a step sees what the steps
// before it wrote exactly as a later pass would. With 'fold' false every step is a pass under its own name (the A/B
// path: the tracks' *.fold_small_passes switches).
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

    // A step: a pass's setup and execute as for RenderGraph::addPass. Folding, the setup runs at flush(): what it
    // captures by reference must live until then (flush in the scope that added the steps).
    void add(std::string name, RenderGraph::SetupFn setup, RenderGraph::ExecuteFn execute)
    {
        if (!m_fold)
        {
            m_graph.addPass(name, m_queue, setup, std::move(execute));
            return;
        }
        m_setups.push_back(std::move(setup));
        m_executes.push_back(std::move(execute));
    }

    // The steps added since the last flush as one pass 'name' (nothing to do without steps, or when not folding).
    void flush(const std::string& name)
    {
        if (m_setups.empty()) return;
        std::vector<RenderGraph::ExecuteFn> executes = std::move(m_executes);
        m_executes.clear();
        m_graph.addPass(name, m_queue,
                        [&](PassBuilder& b) {
                            for (const RenderGraph::SetupFn& setup : m_setups) setup(b);
                        },
                        [executes = std::move(executes)](PassContext& c) {
                            for (size_t i = 0; i < executes.size(); ++i)
                            {
                                if (i > 0)
                                {
                                    D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                             D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                                    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                                    group.pGlobalBarriers = &gb;
                                    c.cmd->Barrier(1, &group);
                                }
                                executes[i](c);
                            }
                        });
        m_setups.clear();
    }

private:
    RenderGraph& m_graph;
    QueueType m_queue;
    bool m_fold;
    std::vector<RenderGraph::SetupFn> m_setups;
    std::vector<RenderGraph::ExecuteFn> m_executes;
};
} // namespace unx::render
