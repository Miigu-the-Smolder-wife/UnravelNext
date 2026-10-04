// Immutable fluid publication boundary; real bridge COPY submissions/readbacks,
// retained publication pins and a deliberately unfinished GPU reader. No render
// frame or fluid solver approximation is needed to verify these byte lifetimes.
#include "Renderer/HostRenderer.h"
#include "GpuBridge/GpuBridge.h"
#include "TestScenes.h"
#include "unx/core/File.h"
#include <algorithm>
#include <cstring>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace {
struct View {
    uint32_t size=sizeof(View),version=1;
    void* current=nullptr;void* start=nullptr;
    uint64_t currentResource=0,startResource=0;
    uint32_t count=4,startCount=3,stride=80,startValid=1;
    double origin[3]{};float dx=.05f;uint32_t reserved=0;uint64_t tick=1;
};
static_assert(sizeof(View)==96);
struct Lease {
    NRC_GpuBridge value;
    explicit Lease(GpuBridgeHost& host):value(host.acquire()){}
    ~Lease(){value.release(value.context);}
};
struct Buffer {
    NRC_GpuBridge& bridge;ComPtr<ID3D12Resource> resource;uint64_t id=0;
    Buffer(NRC_GpuBridge& table,uint64_t bytes,D3D12_HEAP_TYPE type):bridge(table){
        auto* device=static_cast<ID3D12Device*>(bridge.device);
        const auto state=type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:
            type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON;
        D3D12_HEAP_PROPERTIES heap{type};D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"test buffer");
        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        check(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE,&architecture,sizeof architecture),"test architecture");
        NRC_GpuAllocation allocation{};allocation.size=sizeof allocation;allocation.version=1;allocation.owner=0x465054;
        allocation.bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;allocation.domain=NRC_GPU_PHYSICS_DOMAIN;
        allocation.memory=type==D3D12_HEAP_TYPE_UPLOAD?NRC_GPU_UPLOAD_MEMORY:type==D3D12_HEAP_TYPE_READBACK?NRC_GPU_READBACK:NRC_GPU_PERSISTENT;
        allocation.nonlocal=type!=D3D12_HEAP_TYPE_DEFAULT&&!architecture.UMA?1u:0u;
        if(bridge.reserve(bridge.context,&allocation,&id)!=NRC_GPU_OK||!id)fail("test reserve");
        if(bridge.bind(bridge.context,id,resource.Get(),state)!=NRC_GPU_OK)fail("test bind");
    }
    Buffer(const Buffer&)=delete;
    ~Buffer(){if(id)bridge.release_resource(bridge.context,id);}
    void retire(){if(id){if(bridge.release_resource(bridge.context,id)!=NRC_GPU_OK)fail("test retire");id=0;}}
};
struct Recorder {
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    explicit Recorder(ID3D12Device* device){
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&allocator)),"test allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"test list");
    }
};
NRC_GpuWorldStamp sourceStamp(){NRC_GpuWorldStamp s{};s.world=77;s.world_generation=1;s.epoch=1;s.tick=1;s.branch=1;s.phase=1;return s;}
NRC_GpuFence copy(HostRenderer& host,ID3D12Resource* from,uint64_t fromId,ID3D12Resource* to,uint64_t toId,uint64_t bytes){
    Recorder commands(host.d3dDevice());commands.list->CopyBufferRegion(to,0,from,0,bytes);check(commands.list->Close(),"test close");
    return host.gpuBridge().submitPresentationCopy(commands.list.Get(),commands.allocator.Get(),{{fromId,toId,bytes}},sourceStamp());
}
void wait(NRC_GpuBridge& bridge,NRC_GpuFence fence){uint32_t ready=0;if(bridge.poll(bridge.context,fence,10000,&ready)!=NRC_GPU_OK||!ready)fail("test completion timeout");}
std::vector<uint32_t> pattern(uint32_t seed,uint32_t count=80){std::vector<uint32_t> words(count);for(uint32_t i=0;i<count;++i)words[i]=seed^(i*2654435761u);return words;}
NRC_GpuFence write(HostRenderer& host,NRC_GpuBridge& bridge,Buffer& destination,const std::vector<uint32_t>& words){
    Buffer upload(bridge,words.size()*4,D3D12_HEAP_TYPE_UPLOAD);void* mapped=nullptr;
    check(upload.resource->Map(0,nullptr,&mapped),"test upload map");std::memcpy(mapped,words.data(),words.size()*4);upload.resource->Unmap(0,nullptr);
    return copy(host,upload.resource.Get(),upload.id,destination.resource.Get(),destination.id,words.size()*4);
}
std::vector<uint32_t> mapped(Buffer& readback,uint32_t words){
    void* data=nullptr;D3D12_RANGE read{0,size_t(words)*4};check(readback.resource->Map(0,&read,&data),"test readback map");
    std::vector<uint32_t> result(words);std::memcpy(result.data(),data,size_t(words)*4);D3D12_RANGE none{};readback.resource->Unmap(0,&none);return result;
}
std::vector<uint32_t> read(HostRenderer& host,NRC_GpuBridge& bridge,ID3D12Resource* source,uint64_t id,uint32_t words){
    Buffer readback(bridge,uint64_t(words)*4,D3D12_HEAP_TYPE_READBACK);
    wait(bridge,copy(host,source,id,readback.resource.Get(),readback.id,uint64_t(words)*4));return mapped(readback,words);
}
} // namespace

int main(){
    try{
        uint32_t checks=0;
        auto expect=[&](bool condition,const char* message){++checks;if(!condition)fail("fluid presentation: %s",message);};
        auto rejects=[](auto&& action){try{action();return false;}catch(const std::exception&){return true;}};
        HostRendererOptions options;options.standalone=true;options.framesInFlight=2;options.debugLayer=true;
        options.shaderDirectory=executableDirectory()/"shaders";options.qualityDirectory=std::filesystem::path(UNX_SOURCE_DIR)/"Config/quality";
        HostRenderer host(options);host.scene()=unx::host::test::oneBox();auto water=host.scene().materials[0];water.cls=scene::MaterialClass::Water;water.ior=1.33f;host.scene().materials.push_back(water);host.commit();
        Lease lease(host.gpuBridge());auto& bridge=lease.value;
        Buffer current(bridge,320,D3D12_HEAP_TYPE_DEFAULT),start(bridge,240,D3D12_HEAP_TYPE_DEFAULT);
        const auto a=pattern(0x1234),aStart=pattern(0x5678,60),b=pattern(0x9abc),bStart=pattern(0xdef0,60),c=pattern(0x1357),d=pattern(0x2468);
        write(host,bridge,current,a);write(host,bridge,start,aStart);
        View view;view.current=current.resource.Get();view.start=start.resource.Get();view.currentResource=current.id;view.startResource=start.id;
        HostRenderer::FluidInput input;input.view=&view;input.alpha=.25f;input.material=1;input.domainCells[0]=input.domainCells[1]=input.domainCells[2]=16;
        uint64_t stamp[6]{77,1,1,1,1,1};
        host.setFluids({&input,1},stamp);auto first=host.queuedFluids();
        expect(first&&first->size()==1,"no queued publication");
        const auto firstCurrent=first->front().currentResource,firstStart=first->front().startResource;
        ComPtr<ID3D12Resource> firstBuffer=first->front().frame.current;
        expect(firstCurrent!=current.id&&firstStart!=start.id,"presentation borrowed authoritative buffer IDs");
        // Producer overwrites the SAME IDs immediately. The queued snapshot must
        // still contain both original histories after all work completes.
        write(host,bridge,current,b);write(host,bridge,start,bStart);
        expect(read(host,bridge,first->front().frame.current,firstCurrent,80)==a,"current history changed after producer overwrite");
        expect(read(host,bridge,first->front().frame.start,firstStart,60)==aStart,"start history changed after producer overwrite");
        const auto beforeAlpha=host.gpuBridge().statistics().submissions[NRC_GPU_COPY];input.alpha=.875f;
        host.setFluids({&input,1},stamp);auto alpha=host.queuedFluids();
        expect(alpha->front().currentResource==firstCurrent&&alpha->front().presentation==first->front().presentation&&alpha->front().frame.alpha==.875f,"alpha replaced immutable snapshot");
        expect(host.gpuBridge().statistics().submissions[NRC_GPU_COPY]==beforeAlpha,"alpha submitted another GPU copy");
        ++stamp[2];++stamp[3];++view.tick;host.setFluids({&input,1},stamp);auto second=host.queuedFluids();
        expect(second->front().currentResource!=firstCurrent,"retained CPU publication was reused");
        expect(read(host,bridge,second->front().frame.current,second->front().currentResource,80)==b,"new tick did not receive new current bytes");
        expect(read(host,bridge,second->front().frame.start,second->front().startResource,60)==bStart,"new tick did not receive new start bytes");
        expect(read(host,bridge,firstBuffer.Get(),firstCurrent,80)==a,"new publication overwrote retained old publication");
        first.reset();alpha.reset();
        // Only the cache owns publication one now. A submitted but incomplete
        // reader, without a CPU publication pin, must still prevent its reuse.
        ComPtr<ID3D12Fence> gate;check(host.d3dDevice()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"test reader gate");
        struct ReleaseGate {ID3D12Fence* fence;~ReleaseGate(){fence->Signal(1);}} releaseGate{gate.Get()};
        check(static_cast<ID3D12CommandQueue*>(bridge.copy_queue)->Wait(gate.Get(),1),"hold presentation reader");
        Buffer heldReadback(bridge,320,D3D12_HEAP_TYPE_READBACK);
        const auto heldRead=copy(host,firstBuffer.Get(),firstCurrent,heldReadback.resource.Get(),heldReadback.id,320);
        uint32_t ready=1;expect(bridge.poll(bridge.context,heldRead,0,&ready)==NRC_GPU_OK&&!ready,"reader gate did not hold GPU work");
        expect(!host.gpuBridge().resourcesIdle({firstCurrent,firstStart}),"unfinished reader was reported idle");
        write(host,bridge,current,c);++stamp[2];++stamp[3];++view.tick;host.setFluids({&input,1},stamp);auto third=host.queuedFluids();
        expect(third->front().currentResource!=firstCurrent&&third->front().currentResource!=second->front().currentResource,"reader fence or retained publication did not prevent reuse");
        check(gate->Signal(1),"release presentation reader");wait(bridge,heldRead);
        expect(mapped(heldReadback,80)==a,"in-flight reader saw recycled bytes");
        expect(read(host,bridge,third->front().frame.current,third->front().currentResource,80)==c,"third publication copied wrong producer version");
        expect(host.gpuBridge().resourcesIdle({firstCurrent,firstStart}),"completed reader did not retire");
        write(host,bridge,current,d);++stamp[2];++stamp[3];++view.tick;host.setFluids({&input,1},stamp);auto fourth=host.queuedFluids();
        expect(fourth->front().currentResource==firstCurrent,"unreferenced completed publication was not recycled");
        expect(read(host,bridge,fourth->front().frame.current,fourth->front().currentResource,80)==d,"recycled publication copied stale bytes");
        // Frozen publications survive producer retirement. Reusing their exact
        // key is valid; a different pointer or a fresh stamp with stale IDs is not.
        current.retire();start.retire();host.gpuBridge().collect();
        const auto retiredCopies=host.gpuBridge().statistics().submissions[NRC_GPU_COPY];input.alpha=.5f;host.setFluids({&input,1},stamp);
        expect(host.queuedFluids()->front().presentation==fourth->front().presentation&&host.gpuBridge().statistics().submissions[NRC_GPU_COPY]==retiredCopies,"producer retirement invalidated owned frozen publication");
        Buffer replacement(bridge,320,D3D12_HEAP_TYPE_DEFAULT);write(host,bridge,replacement,a);
        View forged=view;forged.current=replacement.resource.Get();auto invalid=input;invalid.view=&forged;
        expect(rejects([&]{host.setFluids({&invalid,1},stamp);}),"pointer substitution reused a stale cache key");
        uint64_t nextStamp[6];std::copy(std::begin(stamp),std::end(stamp),nextStamp);++nextStamp[2];++nextStamp[3];
        expect(rejects([&]{host.setFluids({&input,1},nextStamp);}),"fresh copy accepted retired source IDs");
        expect(host.queuedFluids()->front().currentResource==fourth->front().currentResource,"rejected set replaced the previous publication");
        view.current=replacement.resource.Get();view.currentResource=replacement.id;view.start=nullptr;view.startValid=0;view.startCount=0;view.startResource=0;view.tick=nextStamp[3];
        host.setFluids({&input,1},nextStamp);auto reset=host.queuedFluids();
        expect(read(host,bridge,reset->front().frame.current,reset->front().currentResource,80)==a&&!reset->front().frame.startValid&&!reset->front().frame.start,"reset source identity or absent history was not handled");
        expect(read(host,bridge,fourth->front().frame.current,fourth->front().currentResource,80)==d,"reset invalidated retained pre-reset publication");
        expect(host.debugErrors()==0,"D3D12 debug layer reported errors");
        host.setFluids({},nextStamp);expect(!host.queuedFluids(),"empty input kept queued fluids");
        logf("HOST FLUID PRESENTATION TEST PASS (%u checks, actual COPY queue bytes/fences, immutable current+start, alpha reuse, reader-gated reuse, source retirement/reset, debug errors 0)\n",checks);
        return 0;
    }catch(const std::exception& error){logf("HOST FLUID PRESENTATION TEST ERROR: %s\n",error.what());return 1;}
}
