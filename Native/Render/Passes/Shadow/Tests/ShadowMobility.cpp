// Source-triangle shadow images with the production cache/set predicates. Every
// frame is compared to a forced full redraw with identical projection history, including motion and settling,
// hide/reveal, teleport, wind and origin rebase. The separate GPU probe executes
// the real Visibility instanceInSet for intrinsic/runtime/GPU-only records too.
#include "TestRaster.h"
#include "VsmSystem.h"
#include "unx/clusterbuilder/ClusterBuilder.h"
#include <array>
#include <cstring>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace {
void require(bool value,const char* reason){if(!value)fail("shadow mobility: %s",reason);}
scene::Scene scene(bool local){
    scene::Scene s;s.name="observed shadow mobility";s.materials.push_back({});
    s.meshes.push_back(boxMesh("ground",{12,.1f,12}));s.meshes.push_back(boxMesh("box",{.6f,.8f,.6f}));
    const auto flags=scene::InstanceCastShadow|scene::InstanceDynamic;
    s.instances.push_back(instanceAt(0,{0,-.1f,0},flags));
    for(int z=0;z<3;++z)for(int x=0;x<3;++x)s.instances.push_back(instanceAt(1,{float(x*3-3),.8f,float(z*3-3)},flags));
    s.sun.direction=normalize(float3{-.45f,.8f,-.35f});s.sun.angularRadius=0;
    if(local){
        // Independent point and finite-area local lights exercise the local
        // static copies and their six-face page sets, not only the sun atlas.
        s.sun.direction=normalize(float3{.3f,-.6f,.2f});
        scene::Light point;point.type=scene::LightType::Point;point.position={-2,4,-1};point.intensity=2000;point.range=14;point.castShadow=true;s.lights.push_back(point);
        auto sphere=point;sphere.type=scene::LightType::Sphere;sphere.position={3,3.5f,2};sphere.size={.1f,0};s.lights.push_back(sphere);
    }
    scene::Camera camera;camera.position={-7,6,-9};camera.forward=normalize(float3{.5f,-.4f,.8f});s.cameras.push_back(camera);return s;
}
struct Fixture {
    TestFrame frame;TestRaster raster;uint64_t sequence=0;scene::Camera camera;uint64_t staticGroups=0,dynamicGroups=0;bool forceRedraw;
    bool knownGpuEmpty;uint32_t rasterRequests=0;
    explicit Fixture(bool cached,bool local,uint32_t gpuCapacity=2,bool knownEmpty=false):frame(true),raster(frame),forceRedraw(!cached),knownGpuEmpty(knownEmpty){
        // cache=false also changes the padded/retained SUN projection range,
        // which is not a byte-identical reference at precision edges after a
        // rebase. Keep that projection policy identical, and force every page
        // to redraw through the existing invalidation flag. VsmSystem reads
        // this flag only for page reuse, never while deriving projection bounds.
        frame.quality.applyOverride("shadow.vsm.cache=true");
        frame.quality.applyOverride(std::string("shadow.vsm.static_separate=")+(cached?"true":"false"));
        frame.quality.applyOverride("shadow.vsm.local_static_separate=true");
        frame.quality.applyOverride("shadow.vsm.cache_min_change_texels=0");
        // Four reserved-pool views per request: exercises real splitting while fitting the test helper's 16 CBV slots.
        if(gpuCapacity>2){frame.quality.applyOverride("visibility.max_visible_clusters=262144");frame.quality.applyOverride("shadow.vsm.raster_split=true");}
        frame.gpuScene.reserveRuntime(RuntimeCapacity{.instances=2,.gpuInstances=gpuCapacity});
        frame.setScene(scene(local));camera=frame.sceneData.cameras[0];frame.frame.deltaTime=1.f/60;
        if(gpuCapacity>2){frame.gpuScene.setClusters(clusterbuilder::build(frame.sceneData,clusterbuilder::Settings::fromQuality(frame.quality)));
            require(frame.gpuScene.meshes()[0].clusterCount>0&&frame.gpuScene.gpuInstanceRange().capacity==gpuCapacity,"producer-bound fixture lacks actual cluster capacity");}
        frame.testServices.rasterizeDepth=[this](FramePassContext& fc,const DepthRasterRequest& request){
            ++rasterRequests;
            for(const auto& view:request.views){if(view.instanceSet==1)++staticGroups;else if(view.instanceSet==2)++dynamicGroups;}
            raster.rasterizeDepth(fc,request);
        };
    }
    std::vector<uint8_t> render(){
        rasterRequests=0;
        frame.gpuScene.flushUpdates(frame.frame.frameIndex,2,frame.shaders);
        frame.frame.discontinuity=forceRedraw?kDiscontinuityRestore:0;
        frame.frame.mainView=ViewDesc::fromCamera(camera,256,192,float4x4{});frame.frame.mainView.prevViewProj=frame.frame.mainView.viewProj;
        std::shared_ptr<std::vector<uint8_t>> visibility;
        frame.run([&](FramePassContext& fc){ViewResources main;main.view=fc.frame.mainView;main.frameConstants=fc.frameConstantsFor(main.view);
            if(knownGpuEmpty)fc.resources.gpuInstanceClusterBound=0; // the sole producer emitted no GPU instances after flush's count clear
            raster.mainView(fc,main);tracks::shadowPages(fc,main);tracks::shadowVisibility(fc,main);visibility=frame.readback(fc,main.shadowVisibility);});
        ++sequence;frame.frame.time+=frame.frame.deltaTime;frame.frame.originShift={};return *visibility;
    }
    void move(uint32_t id,float3 at,uint32_t flags=0){InstanceTransformUpdate update{id,float3x4::translation(at),flags};frame.gpuScene.updateTransforms(frame.frame.frameIndex,{&update,1});}
};
void predicates(TestFrame& tf){
    gpu::Instance rigid{};rigid.mesh=0;rigid.flags=scene::InstanceCastShadow|scene::InstanceDynamic;
    rigid.bonePalette=rigid.morph=rigid.patch=gpu::kNone;
    rigid.objectToWorld[0]=rigid.prevObjectToWorld[0]={1,0,0,0};rigid.objectToWorld[1]=rigid.prevObjectToWorld[1]={0,1,0,0};rigid.objectToWorld[2]=rigid.prevObjectToWorld[2]={0,0,1,0};
    std::vector<gpu::Instance> records;std::vector<uint2> ids;std::vector<uint32_t> expected;
    auto add=[&](gpu::Instance value,uint32_t id,uint32_t end,bool movable){require(gpu::instanceShadowMovable(value,id,end)==movable,"CPU mobility predicate");records.push_back(value);ids.push_back({id,end});expected.push_back(movable?1u:0u);};
    add(rigid,0,10,false);
    auto changed=rigid;changed.objectToWorld[0].w=.00001f;add(changed,0,10,true);
    for(uint32_t flags:std::array<uint32_t,4>{scene::InstanceSkinned,scene::InstanceWind,gpu::kInstanceViewModel,gpu::kInstanceMotionBreak}){auto value=rigid;value.flags|=flags;add(value,0,10,true);}
    for(int kind=0;kind<3;++kind){auto value=rigid;if(kind==0)value.bonePalette=0;else if(kind==1)value.morph=0;else value.patch=0;add(value,0,10,true);}
    auto hidden=rigid;hidden.flags|=gpu::kInstanceHidden;add(hidden,0,10,false);
    add(rigid,10,10,true);add(rigid,11,10,true); // runtime/GPU records never rely on CPU previous transforms
    auto rebase=rigid;for(int row=0;row<3;++row){rebase.objectToWorld[row].w-=1024;rebase.prevObjectToWorld[row].w-=1024;}add(rebase,0,10,false);
    std::shared_ptr<std::vector<uint8_t>> result;
    tf.run([&](FramePassContext& fc){
        const auto input=tf.uploadBuffer(fc,records.data(),records.size()*sizeof(records[0]),sizeof(records[0]),"mobility records");
        const auto identity=tf.uploadBuffer(fc,ids.data(),ids.size()*sizeof(ids[0]),sizeof(ids[0]),"mobility identities");
        const auto output=fc.graph.createBuffer(BufferDesc{"mobility output",records.size()*16,16});
        auto* pso=fc.shaders.compute("Passes/Shadow/Tests/ShadowMobilityProbe");
        fc.graph.addPass("mobility predicates",QueueType::Compute,[&](PassBuilder& b){b.use(input,Use::SrvCompute);b.use(identity,Use::SrvCompute);b.use(output,Use::UavCompute);},
            [=](PassContext& c){const uint32_t words[]{c.srv(input),c.srv(identity),c.uav(output),uint32_t(records.size())};c.cmd->SetPipelineState(pso);c.computeConstants(words,4);c.cmd->Dispatch(1,1,1);});
        result=tf.readbackBuffer(fc,output,records.size()*16);
    });
    for(size_t i=0;i<records.size();++i){uint32_t words[4];std::memcpy(words,result->data()+i*16,16);require(words[0]==expected[i]&&words[1]==1-expected[i]&&words[2]==expected[i],"GPU VSM/Visibility predicate differs from CPU budgets");}
}
}
int main(){
    try{
        uint32_t comparisons=0;
        {
            Fixture bounded(true,false,65536,true),reserved(true,false,65536,false);
            for(int frame=0;frame<3;++frame){const auto a=bounded.render(),b=reserved.render();
                require(a==b,"producer-bounded shadow differs from reserved-capacity reference");
                require(bounded.rasterRequests<reserved.rasterRequests,"empty reserved GPU pool still splits shadow raster chains");
                logf("  empty GPU pool frame %d: %u -> %u raster requests; %zu exact visibility bytes\n",frame,reserved.rasterRequests,bounded.rasterRequests,a.size());++comparisons;}
            require(bounded.frame.device.drainDebugMessages()==0&&reserved.frame.device.drainDebugMessages()==0,"producer-bound D3D12 errors");
        }
        for(bool local:{false,true}){
        Fixture cached(true,local),fresh(false,local);
        logf("  comparing %s shadow cache against fresh raster\n",local?"local point+sphere":"sun");
        auto compare=[&](const char* name){const auto a=cached.render(),b=fresh.render();require(a.size()==b.size(),"visibility extent changed");
            const auto& ca=shadow::lastConstants(cached.frame.trackState);const auto& cb=shadow::lastConstants(fresh.frame.trackState);
            // Assert equal projection inputs BEFORE comparing any image bytes.
            require(std::memcmp(&ca.lightX,&cb.lightX,48)==0&&std::memcmp(ca.cameraUV,cb.cameraUV,sizeof ca.cameraUV)==0&&
                std::memcmp(ca.level,cb.level,sizeof ca.level)==0,"cached/full-redraw sun projections differ");
            size_t differences=0;for(size_t i=0;i<a.size();++i)differences+=a[i]!=b[i];
            if(differences){
                size_t printed=0;for(size_t i=0;i<a.size()&&printed<8;++i)if(a[i]!=b[i]){logf("    pixel(%zu,%zu) channel%zu cached%u fresh%u\n",(i/4)%256,(i/4)/256,i%4,unsigned(a[i]),unsigned(b[i]));++printed;}
                logf("    sun ranges cached %.9g..%.9g fresh %.9g..%.9g; local active %u/%u\n",ca.hMin,ca.hMax,cb.hMin,cb.hMax,shadow::stats(cached.frame.trackState).localActive,shadow::stats(fresh.frame.trackState).localActive);
            }
            logf("  %-30s visibility bytes %zu, differing %zu\n",name,a.size(),differences);require(differences==0,"cached visibility differs from fresh raster");++comparisons;};
        compare("first authored-Dynamic");compare("rigid cached");compare("rigid steady");
        require(!gpu::instanceShadowMovable(cached.frame.gpuScene.instances()[1],1,cached.frame.gpuScene.staticInstanceCount()),"unchanged Dynamic caster did not become cacheable");
        for(auto* f:{&cached,&fresh})f->move(1,{-1,.8f,-3});compare("rigid movement");compare("settled rigid");compare("settled cache reused");
        for(auto* f:{&cached,&fresh})f->frame.gpuScene.setInstanceVisible(1,false);compare("hidden caster");compare("hidden cache");
        for(auto* f:{&cached,&fresh})f->frame.gpuScene.setInstanceVisible(1,true);compare("revealed caster");
        for(auto* f:{&cached,&fresh})f->move(1,{2,.8f,-3},kTransformTeleport);compare("teleport old+new bounds");compare("teleport settled");
        for(auto* f:{&cached,&fresh}){f->frame.sceneData.instances[2].flags|=scene::InstanceWind;f->frame.sceneData.instances[2].wind.stiffness=.1f;f->frame.sceneData.windSpeed=5;const uint32_t id=2;f->frame.gpuScene.setInstances({&id,1});}
        compare("wind intrinsic movement");compare("wind next frame");
        std::array<uint32_t,2> runtime{};size_t at=0;
        for(auto* f:{&cached,&fresh}){runtime[at++]=f->frame.gpuScene.addRuntimeInstance(instanceAt(1,{1,.8f,1}));require(runtime[at-1]!=gpu::kNone,"runtime instance allocation");}
        compare("runtime caster");
        at=0;for(auto* f:{&cached,&fresh})f->move(runtime[at++],{2,.8f,1});compare("runtime caster movement");compare("runtime remains movable");
        for(auto* f:{&cached,&fresh}){const float3 shift{1024,0,-1024};f->frame.gpuScene.rebase(shift);f->camera.position=f->camera.position-shift;f->frame.frame.originShift=shift;}
        compare("origin rebase");compare("rebased cache");
        require(cached.staticGroups&&cached.dynamicGroups,"test did not exercise both actual raster sets");
        const auto& full=shadow::stats(fresh.frame.trackState);
        require(full.requested>0&&full.cachedPages==0&&full.dirty==full.requested,"reference unexpectedly reused shadow pages");
        if(local)require(shadow::stats(cached.frame.trackState).localAssigned>=2&&shadow::stats(cached.frame.trackState).localActive>=2,"local-light test did not keep both shadow slots active after rebase");
        predicates(cached.frame);
        require(cached.frame.device.drainDebugMessages()==0&&fresh.frame.device.drainDebugMessages()==0,"D3D12 debug errors");
        }
        logf("SHADOW MOBILITY TEST PASS (%u complete sun/local cached/fresh images; CPU/GPU set parity including runtime/GPU, skin, wind, morph, patch, motion break)\n",comparisons);return 0;
    }catch(const std::exception& error){logf("SHADOW MOBILITY TEST ERROR: %s\n",error.what());return 1;}
}
