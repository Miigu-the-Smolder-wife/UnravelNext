#include "../../Material/Tests/MTestFrame.h"
#include <array>
#include <bit>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
static uint32_t asU(float v) { return std::bit_cast<uint32_t>(v); }

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back();
        scene.cameras[0].position = {0.2f, 0.1f, 3}; scene.cameras[0].forward = normalize(float3{-0.05f, -0.03f, -1});
        scene::Mesh mesh; mesh.positions = {{-2, -1, 0}, {2, -1, 0}, {2, 1, 0}, {-2, 1, 0}};
        mesh.normals.assign(4, float3{0, 0, 1}); mesh.tangents.assign(4, float4{1, 0, 0, 1});
        mesh.uv0 = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; mesh.indices = {0, 1, 2, 0, 2, 3}; mesh.submeshes.push_back({0, 6, 0});
        scene.meshes.push_back(mesh); scene.instances.emplace_back(); test.setScene(scene);
        const uint32_t zero[16] = {};
        auto planarResource = uploadStatic(test.device, zero, sizeof zero, L"test empty planar table");
        uint64_t checked = 0, changed = 0; double worst = 0;
        for (uint32_t mode = 0; mode < 8; ++mode)
        {
            auto& material = test.sceneData.materials[0];
            material.roughness = mode == 0 ? 0.03f : mode == 1 ? 0.08f : 0.35f;
            material.clearcoat = mode == 4 ? 0.5f : 0; material.clearcoatRoughness = 0.18f;
            const uint32_t mi = 0; test.gpuScene.setMaterials({&mi, 1});
            const uint32_t W = mode & 1 ? 513 : 960, H = mode & 1 ? 257 : 540;
            const uint32_t factor = mode >= 3 ? 2 : 1, frame = 32767 + mode * 83;
            const uint32_t downsample = factor | ((frame & 1u) << 8) | (((frame >> 1) & 1u) << 16);
            test.frame.frameIndex = frame;
            std::array<std::shared_ptr<std::vector<uint8_t>>, 2> output;
            test.run([&](FramePassContext& fc) {
                auto view = test.mainView(fc, W, H); test.vis.record(fc, view); tracks::materialResolve(fc, view);
                const auto words = material::resolveOutputs(fc, view).materialWord;
                auto& g = fc.graph; auto& shaders = fc.shaders;
                const auto modes = g.createTexture({"test modes", W, H, 1, 1, DXGI_FORMAT_R32_UINT});
                const auto values = g.createBuffer({"test ray results", uint64_t(W)*H*12, 12});
                const auto reflection = g.createTexture({"test reflection", W, H+(H+7)/8, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT});
                const auto cache = g.createBuffer({"test ray cache", uint64_t(W)*H*16, 0});
                const auto constants = fc.frameConstantsFor(view.view);
                if (mode >= 6)
                {
                    const auto jobs = g.createBuffer({"test classified jobs", uint64_t(W)*H*4, 4});
                    const auto counter = g.createBuffer({"test classify counter", 64, 0});
                    const auto planar = g.importBuffer(planarResource.Get(), {"test planar", sizeof zero, 0});
                    g.addPass("test.counter",QueueType::Compute,[&](PassBuilder& b){b.use(counter,Use::UavCompute);},[=,&shaders](PassContext& c){
                        const uint32_t k[4]={0,0,c.uav(counter),16};
                        c.cmd->SetPipelineState(shaders.compute("Passes/Water/ViewGridClear"));c.computeConstants(k,4);gpuDispatch(c.cmd,1,1,1);
                    });
                    g.addPass("test.classify",QueueType::Compute,[&](PassBuilder& b){
                        b.use(view.depth,Use::SrvCompute);b.use(view.gbuffer,Use::SrvCompute);b.use(words,Use::SrvCompute);b.use(planar,Use::SrvCompute);
                        b.use(modes,Use::UavCompute);b.use(jobs,Use::UavCompute);b.use(counter,Use::UavCompute);b.use(reflection,Use::UavCompute);b.use(cache,Use::UavCompute);
                    },[=,&shaders](PassContext& c){
                        uint32_t k[36]={c.srv(view.depth),c.srv(view.gbuffer),UINT32_MAX,c.srv(view.depth),c.uav(modes),c.uav(jobs),c.uav(counter),c.uav(reflection),
                            0,asU(0.05f),asU(float(W)),H,W,H,c.srv(planar),0,c.uav(counter),3,1,asU(1.0f)};
                        k[28]=c.srv(words);k[29]=downsample;k[30]=asU(1.0f);k[31]=c.uav(cache);k[32]=asU(0.1f);
                        c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionClassifyCached"));c.computeConstants(k,36);c.bindFrameConstants(constants);gpuDispatch(c.cmd,(W+7)/8,(H+7)/8,1);
                    });
                }
                g.addPass("test.input", QueueType::Compute, [&](PassBuilder& b) {
                    b.use(modes, Use::UavCompute); b.use(values, Use::UavCompute); b.use(reflection, Use::UavCompute); b.use(view.depth, Use::SrvCompute);
                }, [=, &shaders](PassContext& c) {
                    const uint32_t k[12] = {c.uav(modes),c.uav(values),c.uav(reflection),c.srv(view.depth),W,H,factor,frame&3u,mode>=6?1u:0u,0,0,0};
                    c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/Tests/ReuseCacheInput"));
                    c.computeConstants(k,12); c.bindFrameConstants(constants); gpuDispatch(c.cmd,(W+7)/8,(H+7)/8,1);
                });
                if (mode < 6) g.addPass("test.cache", QueueType::Compute, [&](PassBuilder& b) {
                    b.use(modes,Use::SrvCompute); b.use(view.depth,Use::SrvCompute); b.use(view.gbuffer,Use::SrvCompute); b.use(words,Use::SrvCompute); b.use(cache,Use::UavCompute);
                }, [=, &shaders](PassContext& c) {
                    const uint32_t k[12] = {c.srv(modes),c.srv(view.depth),c.srv(view.gbuffer),c.uav(cache),W,H,frame,asU(0.1f),c.srv(words),0,0,0};
                    c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/Tests/ReuseRayReference"));
                    c.computeConstants(k,12); c.bindFrameConstants(constants); gpuDispatch(c.cmd,(W+7)/8,(H+7)/8,1);
                });
                for (uint32_t variant=0; variant<2; ++variant)
                {
                    const auto resolved=g.createTexture({"test resolved",W,H,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT});
                    g.addPass(variant ? "test.cached" : "test.reference", QueueType::Compute, [&](PassBuilder& b) {
                        b.use(modes,Use::SrvCompute);b.use(values,Use::SrvCompute);b.use(view.depth,Use::SrvCompute);b.use(view.gbuffer,Use::SrvCompute);
                        b.use(words,Use::SrvCompute);b.use(reflection,Use::SrvCompute);b.use(cache,Use::SrvCompute);b.use(resolved,Use::UavCompute);
                    },[=,&shaders](PassContext& c){
                        const uint32_t k[20]={c.srv(modes),c.srv(values),c.srv(view.depth),c.srv(view.gbuffer),c.srv(reflection),c.uav(resolved),H,frame,
                            W,H,8,0,asU(8.0f),asU(40.0f),asU(10.0f),asU(0.1f),c.srv(words),UINT32_MAX,downsample,c.srv(cache)};
                        c.cmd->SetPipelineState(shaders.compute(variant ? "Passes/Reflection/ReflectionReuseResolveCached" : "Passes/Reflection/ReflectionReuseResolve"));
                        c.computeConstants(k,20);c.bindFrameConstants(constants);gpuDispatch(c.cmd,(W+7)/8,(H+7)/8,1);
                    });
                    output[variant]=test.readback(fc,resolved);
                }
            });
            for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W*4;++x)
            {
                const size_t offset=y*TestFrame::rowPitch(W,16)+x*4;
                float a,b;std::memcpy(&a,output[0]->data()+offset,4);std::memcpy(&b,output[1]->data()+offset,4);
                const double error=std::abs(double(a)-b)/std::max(1.0,std::abs(double(a)));
                worst=std::max(worst,error);changed+=a!=b;
                M_CHECK(std::isfinite(a)&&std::isfinite(b)&&error<=1e-5,"ray-cache mismatch mode %u pixel %u,%u: %.9g vs %.9g",mode,x/4,y,a,b);
            }
            checked+=uint64_t(W)*H;
        }
        logf("PASS reuse cache: %llu pixels, %llu changed FP32 channels, worst normalized error %.9g; production classifier, mirror/gloss/coat, full/half ray grid, holes and odd edges; GBV enabled\n",
            (unsigned long long)checked,(unsigned long long)changed,worst);
        return 0;
    }
    catch(const std::exception& e){logf("FAIL %s\n",e.what());return 1;}
}
