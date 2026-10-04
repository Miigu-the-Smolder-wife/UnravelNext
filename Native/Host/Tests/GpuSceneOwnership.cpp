// Scene-owner GPU fences: unrelated compute progresses behind a blocked graphics
// tail; real bindless scene consumers wait for writes; writes wait for actual readers.
#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include <cstring>
#include <string>

using namespace unx;
using namespace unx::render;
namespace {
void require(bool condition,const char* message){if(!condition)fail("Scene ownership: %s",message);}
bool reached(Queue& queue,uint64_t value,DWORD timeout=2000)
{
    if(queue.completed()>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    require(event!=nullptr,"CreateEvent");check(queue.fence()->SetEventOnCompletion(value,event),"scene test fence event");
    const DWORD result=WaitForSingleObject(event,timeout);CloseHandle(event);return result==WAIT_OBJECT_0;
}
struct Block
{
    Device& device;ComPtr<ID3D12Fence> gate;bool released=false;
    Block(Device& device,Queue& queue):device(device)
    {
        check(device.d3d()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"scene test gate");
        check(queue.get()->Wait(gate.Get(),1),"block unrelated queue tail");
    }
    void release(){check(gate->Signal(1),"release scene test gate");released=true;device.waitIdle();}
    ~Block(){if(!released){gate->Signal(1);device.waitIdle();}} // failure must not leave a queue blocked during teardown
};
ComPtr<ID3D12Resource> readback(Device& device)
{
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=sizeof(gpu::Instance);desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device.d3d()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"scene test readback");return result;
}
void readScene(RenderGraph& graph,GpuScene& scene,const ComPtr<ID3D12Resource>& output,QueueType queue=QueueType::Compute)
{
    auto* resource=scene.buffer("instances");
    const auto input=graph.importBuffer(resource,{"scene ownership test",resource->GetDesc().Width,sizeof(gpu::Instance)});
    graph.setAsyncCompute(true);
    graph.addPass("scene.real.consumer",queue,[=](PassBuilder& b){b.use(input,Use::CopySrc);b.keep();},
        [=](PassContext& c){c.cmd->CopyBufferRegion(output.Get(),0,c.resource(input),0,sizeof(gpu::Instance));});
}
void expectX(const ComPtr<ID3D12Resource>& output,float x)
{
    void* data=nullptr;D3D12_RANGE range{0,sizeof(gpu::Instance)};check(output->Map(0,&range,&data),"map scene test readback");
    gpu::Instance instance{};std::memcpy(&instance,data,sizeof instance);D3D12_RANGE none{0,0};output->Unmap(0,&none);
    require(instance.objectToWorld[0].w==x,"consumer read a stale or prematurely overwritten scene transform");
}
}
int main(int argc,char** argv)
{
    try
    {
        DeviceOptions options;options.debugLayer=true;
        for(int i=1;i<argc;++i){if(std::string(argv[i])=="--gbv")options.gpuValidation=true;else fail("unknown argument %s",argv[i]);}
        Device device(options);ShaderLibrary shaders(device,executableDirectory()/"shaders");
        const auto source=host::test::oneBox();GpuScene scene(device);scene.upload(source);
        (void)shaders.compute("Passes/Common/SceneUpdate");(void)shaders.compute("Passes/Common/SceneShift");
        Queue& graphics=device.queue(QueueType::Graphics);Queue& compute=device.queue(QueueType::Compute);
        auto move=[&](uint64_t frame,float x){auto transform=source.instances[0].transform;transform.m[0][3]=x;
            const InstanceTransformUpdate update{0,transform,0};scene.updateTransforms(frame,{&update,1});};
        // Enter the tracked-owner contract; future submissions no longer infer unrelated queues as readers.
        {RenderGraph initial(device);scene.flushUpdates(0,2,shaders,&initial);initial.execute(nullptr);device.waitIdle();}
        auto output=readback(device);
        {
            RenderGraph consumer(device);Block tail(device,graphics);
            move(1,3);scene.flushUpdates(1,2,shaders,&consumer);
            const auto independent=compute.signal();require(reached(compute,independent),"scene recording blocked independent FX compute behind graphics tail");
            readScene(consumer,scene,output);consumer.execute(nullptr);
            require(compute.completed()<consumer.lastFence(QueueType::Compute),"actual scene consumer bypassed pending scene update");
            tail.release();expectX(output,3);
        }
        {
            RenderGraph consumer(device);Block tail(device,compute);
            const auto unrelated=compute.signal();
            move(2,7);scene.flushUpdates(2,2,shaders,&consumer);
            const auto written=graphics.lastSignaled();
            require(reached(graphics,written),"scene update waited for unrelated compute tail instead of its last scene reader");
            require(compute.completed()<unrelated,"compute tail was not blocked");
            readScene(consumer,scene,output);consumer.execute(nullptr);tail.release();expectX(output,7);
        }
        {
            RenderGraph reader(device),next(device);Block holdReader(device,compute);
            scene.flushUpdates(3,2,shaders,&reader);readScene(reader,scene,output);reader.execute(nullptr);
            move(4,11);scene.flushUpdates(4,2,shaders,&next);
            require(!reached(graphics,graphics.lastSignaled(),30),"scene writer did not wait for an actual in-flight reader");
            holdReader.release();expectX(output,7);
            readScene(next,scene,output);next.execute(nullptr);device.waitIdle();expectX(output,11);
        }
        {
            auto firstOutput=readback(device);
            // Never execute the abandoned recording, but retain its object until after the gate
            // releases: RenderGraph's destructor waits for device idle even for unsubmitted work.
            // Declare every graph before Block so exception unwinding releases the gate first too.
            RenderGraph discarded(device),observer(device),retry(device);Block tail(device,graphics);
            move(5,13);scene.flushUpdates(5,2,shaders,&discarded);readScene(discarded,scene,output);
            scene.flushUpdates(5,2,shaders,&observer); // no new write; retain the discarded recording's ready fence
            readScene(observer,scene,firstOutput,QueueType::Graphics);observer.execute(nullptr);
            // Read the first queued payload before its replacement. Reusing this frame's mapped
            // upload or descriptor would make the first scatter incorrectly write 17 as well.
            move(5,17);scene.flushUpdates(5,2,shaders,&retry);
            require(reached(compute,compute.signal()),"discarded recording leaked a queue wait into independent simulation");
            readScene(retry,scene,output);retry.execute(nullptr);
            require(compute.completed()<retry.lastFence(QueueType::Compute),"retry forgot its outstanding scene write");
            tail.release();expectX(firstOutput,13);expectX(output,17);
        }
        require(device.drainDebugMessages()==0,"D3D12 debug validation errors");
        logf("Scene ownership PASS: independent FX progress behind graphics tail; scene update/read ordering; unrelated compute-tail independence; discard/retry; debug errors 0.\n");
        return 0;
    }
    catch(const std::exception& error){logf("FAIL: %s\n",error.what());return 1;}
}
