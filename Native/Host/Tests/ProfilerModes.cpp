// Actual GPU timestamp/query-count and calibrated two-queue timeline contracts.
// No performance target: execute only in an approved GPU correctness window.
#include "unx/render/GpuProfiler.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <cstdlib>

using namespace unx;
using namespace unx::render;
namespace {
void require(bool value,const char* message){if(!value)fail("Profiler modes: %s",message);}
}
int main()
{
    try
    {
        char splitValue[2]{}; size_t splitLength=0;
        const bool split=getenv_s(&splitLength,splitValue,sizeof(splitValue),"UNX_GPU_SPLIT_BOUNDARIES")==0 && splitLength==2 && splitValue[0]=='1';
        DeviceOptions options;options.debugLayer=true;
        Device device(options);RenderGraph graph(device);graph.setAsyncCompute(true);
        for(uint64_t slot=0;slot<2;++slot)
        {
        GpuProfiler profiler(device,2,128);
        require(profiler.enabled(),"direct profiler/Harness default must remain detailed");
        const bool modes[]{false,true,false};profiler.setPassTimestamps(modes[0]);profiler.beginFrame(slot);
        for(uint64_t mode=0;mode<3;++mode)
        {
            const uint64_t frame=slot+2*mode;
            uint32_t executed=0;
            for(uint32_t pass=0;pass<100;++pass)
                graph.addPass("profiler.graphics",QueueType::Graphics,[pass](PassBuilder& b){
                    b.keep();if(pass==49)b.fenceAfter([](Queue&,uint64_t){});
                },[&](PassContext&){++executed;});
            graph.addPass("profiler.compute",QueueType::Compute,[](PassBuilder& b){b.keep();},[&](PassContext&){++executed;});
            graph.execute(&profiler);device.waitIdle();
            require(executed==101&&graph.stats().livePasses==101,"profiling mode changed executable pass work");
            profiler.setPassTimestamps(mode+1<3?modes[mode+1]:true);
            profiler.beginFrame(frame+2); // reuse this slot; both slot offsets are tested on each queue
            const auto* timing=profiler.lastCompleted();require(timing&&timing->frame==frame,"completed frame identity");
            require(timing->detailedPassTimings==modes[mode],"in-flight slot lost its recording mode");
            require(timing->queues[0].lists==2&&timing->queues[1].lists==1,"test did not exercise split graphics and async compute");
            require(timing->timestampCount==(modes[mode]?(split?208u:107u):9u),"query count does not match diagnostic mode");
            require(timing->passes.size()==(modes[mode]?(split?202u:101u):0u),"detailed pass/name retention does not match mode");
            if(modes[mode]&&split)
                for(size_t p=0;p<timing->passes.size();p+=2)
                {
                    require(timing->passes[p].name=="prepare:"+timing->passes[p+1].name,"prepare/body pair identity");
                    require(timing->passes[p].endMs==timing->passes[p+1].beginMs,"prepare/body intervals do not meet");
                }
            require(std::isfinite(timing->gpuFrameMs)&&timing->gpuFrameMs>0,"whole GPU interval missing");
            double end=0;
            for(uint32_t q=0;q<2;++q)
            {
                const auto& queue=timing->queues[q];
                require(queue.headMs>=0&&queue.workMs>=0&&queue.tailMs>=0&&queue.gapMs>=0,"negative queue interval");
                end=std::max(end,queue.headMs+queue.workMs+queue.tailMs+queue.gapMs);
                if(modes[mode])
                {
                    double work=0;for(const auto& pass:timing->passes)if(uint32_t(pass.queue)==q)work+=pass.durationMs();
                    require(std::abs(work-queue.workMs)<.001,"detailed pass sum differs from measured queue work");
                }
            }
            require(std::abs(end-timing->gpuFrameMs)<.001,"queue work/tail/gap no longer tile the whole calibrated GPU interval");
        }
        }
        {
            ShaderLibrary shaders(device, executableDirectory() / "shaders");
            auto* pso = shaders.compute("Passes/Test/FillBuffer");
            GpuProfiler profiler(device, 2, 128);
            profiler.setWorkloads(true);
            for (uint64_t frame = 0; frame < 4; ++frame)
            {
                profiler.beginFrame(frame);
                if (frame >= 2)
                {
                    auto* result = profiler.lastCompleted();
                    require(result && result->frame == frame - 2 && result->workloads, "workload slot identity");
                    uint32_t bodies = 0;
                    for (const auto& pass : result->passes)
                    {
                        if (pass.name.starts_with("prepare:")) continue;
                        ++bodies;
                        const auto& w = pass.workload;
                        require(w.dispatches == 1 && w.groups == 3, "recorded dispatch/group counts");
                        require(w.draws == 0 && w.indirect == 0 && w.rayLaunches == 0, "unrelated counters contaminated");
                        require(w.pipelineStatistics == (pass.queue == QueueType::Graphics), "unsupported compute statistics reported as measured");
                        if (w.pipelineStatistics) require(w.computeInvocations == 192, "actual GPU shader invocation count");
                    }
                    require(bodies == 3, "workload mode must break joined scopes into individual passes");
                }
                for (uint32_t n = 0; n < 3; ++n)
                {
                    auto buffer = graph.createBuffer({ "profile-output", 192 * 4, 0 });
                    graph.addPass(n == 2 ? "work.compute" : "work.graphics", n == 2 ? QueueType::Compute : QueueType::Graphics,
                        [=](PassBuilder& b) { b.use(buffer, Use::UavCompute); b.keep(); },
                        [=](PassContext& c) {
                            c.cmd->SetPipelineState(pso);
                            uint32_t constants[]{c.uav(buffer), 192, uint32_t(frame)};
                            c.computeConstants(constants, 3);
                            gpuDispatch(c.cmd, 3, 1, 1);
                        });
                }
                graph.joinPasses(0, 2, "joined.work");
                graph.execute(&profiler);
                device.waitIdle(); // test submission ownership; profiler itself never waits
                require(recordingWorkload == nullptr, "recording scope leaked");
            }
        }
        require(device.drainDebugMessages()==0,"D3D12 debug errors");
        logf("Profiler modes PASS: %u detailed vs 9 frame/queue queries for 101 scopes, accurate two-queue timeline and mode transitions in both readback slots.\n",split?208u:107u);
        return 0;
    }
    catch(const std::exception& e){logf("FAIL: %s\n",e.what());return 1;}
}
