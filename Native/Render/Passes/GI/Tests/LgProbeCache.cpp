#include "../../Material/Tests/MTestFrame.h"
#include "../Lumen/LgProbeCacheLayout.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"
#include <array>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene; scene.cameras.emplace_back(); scene.materials.emplace_back();
        scene.cameras[0].position = {0, 0, -1}; scene.cameras[0].forward = {0, 0, 1};
        // A near wall covering half the field, a far wall, and rays into the sky.
        for (float z : {3.0f, 12.0f})
        {
            scene::Mesh mesh;
            mesh.positions = {{-10,-10,z},{z < 4 ? 0.0f : 10.0f,-10,z},{z < 4 ? 0.0f : 10.0f,10,z},{-10,10,z}};
            mesh.normals.assign(4, {0,0,-1}); mesh.uv0 = {{0,0},{1,0},{1,1},{0,1}};
            mesh.indices = {0,2,1,0,3,2}; mesh.submeshes.push_back({0,6,0});
            scene::Instance instance; instance.mesh = uint32_t(scene.meshes.size());
            scene.meshes.push_back(mesh); scene.instances.push_back(instance);
        }
        test.setScene(scene);
        rt::RayScene rays(test.device, test.shaders, test.gpuScene, test.quality);
        auto& trace = rt::RayPipeline::get(test.device, test.shaders,
            rt::standardRayPipeline("Passes/GI/Tests/LgProbeCacheTrace", {"ProbeCacheTraceGen"}));
        std::array<uint64_t, 9> lookupTotals{}, traceTotals{};
        for (bool async : {false, true})
        {
            test.graph.setAsyncCompute(async);
            for (uint32_t seed = 0; seed < 16; ++seed)
            {
                constexpr uint32_t W = 23, H = 11, count = W * H, uniform = 169;
                const uint32_t extra = seed & 1 ? 13 : count - uniform;
                std::array<std::shared_ptr<std::vector<uint8_t>>, 2> readbacks;
                test.run([&](FramePassContext& fc) {
                    const auto view = test.mainView(fc, W * 8, H * 8);
                    rays.record(fc);
                    uint32_t sceneWords[8]; rays.rootConstants(sceneWords);
                    const auto sceneConstants = std::to_array(sceneWords);
                    const auto params = fc.graph.createBuffer({"test RC params", 256, 0});
                    const auto adaptive = fc.graph.createBuffer({"test adaptive count", 16, 0});
                    const auto lookup = fc.graph.createBuffer({"test prepared lookup", count * LG_PROBE_CACHE_BYTES, 0});
                    const auto indirection = fc.graph.createTexture({"test RC indirection", 16,8,8,1,DXGI_FORMAT_R32_UINT,D3D12_RESOURCE_DIMENSION_TEXTURE3D});
                    const auto atlas = fc.graph.createTexture({"test RC atlas",96,96,1,1,DXGI_FORMAT_R16G16B16A16_FLOAT});
                    const auto depth = fc.graph.createTexture({"test RC depth",64,64,1,1,DXGI_FORMAT_R16_UINT});
                    const auto probeDepth = fc.graph.createTexture({"test probe depth",W,H,1,1,DXGI_FORMAT_R32_FLOAT});
                    const auto position = fc.graph.createTexture({"test probe position",W,H,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT});
                    std::array<BufferRef, 2> stats;
                    std::array<uint32_t, 12> zero{};
                    const auto clear = uploadStatic(test.device, zero.data(), sizeof zero, L"probe cache counters");
                    const auto clearRef = fc.graph.importBuffer(clear.Get(), {"test zero counters",sizeof zero,0});
                    test.keep(clear);
                    for (auto& s : stats) s = fc.graph.createBuffer({"test counters",sizeof zero,0});
                    fc.graph.addPass("test input",QueueType::Compute,
                        [&](PassBuilder& b) {
                            for(auto r:{params,adaptive}) b.use(r,Use::UavCompute);
                            for(auto t:{indirection,atlas,depth,probeDepth,position}) b.use(t,Use::UavCompute);
                            b.use(clearRef,Use::CopySrc); for(auto r:stats)b.use(r,Use::CopyDst);
                        },[=,&fc](PassContext& c) {
                            for(auto s:stats)c.cmd->CopyBufferRegion(c.resource(s),0,c.resource(clearRef),0,sizeof zero);
                            uint32_t k[16]={W,H,seed,c.uav(params),c.uav(indirection),c.uav(atlas),c.uav(depth),c.uav(probeDepth),c.uav(position),c.uav(adaptive),extra};
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/Tests/LgProbeCacheInput"));
                            c.computeConstants(k,16); c.bindFrameConstants(view.frameConstants); c.cmd->Dispatch(12,12,8);
                        });
                    const auto common = [=](PassContext& c,uint32_t* k) {
                        k[32]=W*8;k[33]=H*8;k[34]=W;k[35]=H;k[36]=H;k[37]=8;k[39]=seed;
                        k[40]=uniform;k[41]=count-uniform;k[42]=c.srv(adaptive);k[43]=c.srv(probeDepth);k[45]=c.srv(position);
                    };
                    fc.graph.addPass("test prepare",QueueType::Compute,
                        [&](PassBuilder& b){b.use(params,Use::SrvCompute);b.use(indirection,Use::SrvCompute);b.use(adaptive,Use::SrvCompute);b.use(probeDepth,Use::SrvCompute);b.use(position,Use::SrvCompute);b.use(lookup,Use::UavCompute);},
                        [=,&fc](PassContext& c){uint32_t k[48]={c.srv(params),c.srv(indirection),c.uav(lookup)};common(c,k);
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/Lumen/LgPrepareCache"));c.computeConstants(k,48);c.bindFrameConstants(view.frameConstants);c.cmd->Dispatch((W+7)/8,(H+7)/8,1);});
                    for (uint32_t mode=0;mode<2;++mode)
                    {
                        fc.graph.addPass(mode?"test ray equivalence":"test lookup equivalence",QueueType::Compute,
                            [&](PassBuilder& b){for(auto r:{params,adaptive,lookup})b.use(r,Use::SrvGraphics);for(auto t:{indirection,atlas,depth,probeDepth,position})b.use(t,Use::SrvGraphics);b.use(stats[mode],Use::UavGraphics);if(mode)rays.declareTraversal(b);},
                            [=,&fc,&trace](PassContext& c){uint32_t k[48]={c.srv(params),c.srv(indirection),c.srv(lookup),c.srv(atlas),c.srv(depth),c.uav(stats[mode])};common(c,k);
                                if(mode){std::memcpy(k+24,sceneConstants.data(),sizeof sceneWords);c.computeConstants(k,48);c.bindFrameConstants(view.frameConstants);trace.dispatch(c.cmd,0,count*64,1,1);}
                                else{c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/Tests/LgProbeCacheCompare"));c.computeConstants(k,48);c.bindFrameConstants(view.frameConstants);c.cmd->Dispatch(W,H,1);}});
                        readbacks[mode]=test.readbackBuffer(fc,stats[mode],sizeof zero);
                    }
                });
                for(uint32_t mode=0;mode<2;++mode)
                {
                    std::array<uint32_t,12> s{};std::memcpy(s.data(),readbacks[mode]->data(),sizeof s);
                    for(uint32_t error=0;error<4;++error)M_CHECK(s[error]==0,"probe cache mismatch async %u seed %u stage %u counter %u count %u",async,seed,mode,error,s[error]);
                    auto& totals=mode?traceTotals:lookupTotals;for(uint32_t i=0;i<totals.size();++i)totals[i]+=s[i];
                }
            }
        }
        M_CHECK(lookupTotals[20/4]&&lookupTotals[24/4]&&lookupTotals[28/4],"lookup cases did not exercise all paths");
        M_CHECK(traceTotals[5]&&traceTotals[6]&&traceTotals[7]&&traceTotals[8],"trace cases did not exercise near hits, cache, sky and skipped lookups");
        logf("PASS probe cache: %llu lookup directions, %llu world rays x 3 optimized modes bit-identical in consumed results; %llu skipped cache lookups; missing/untraced/occluded/empty cache, clipmap edges, rebase, inactive rows, sync/async; GPU validation errors 0\n",
            (unsigned long long)lookupTotals[4],(unsigned long long)traceTotals[4],(unsigned long long)traceTotals[8]);
        return 0;
    }
    catch(const std::exception& e){logf("FAIL %s\n",e.what());return 1;}
}
