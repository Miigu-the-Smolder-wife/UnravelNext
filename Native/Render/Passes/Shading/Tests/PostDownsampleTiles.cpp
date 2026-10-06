#include "../../Material/Tests/MTestFrame.h"
#include <array>
#include <bit>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
static uint32_t asU(float f){return std::bit_cast<uint32_t>(f);}

int main()
{
    try
    {
        TestFrame test(true,true);scene::Scene scene;scene.materials.emplace_back();scene.cameras.emplace_back();test.setScene(scene);
        uint64_t pixels=0;
        for(const auto [W,H]:std::array<std::pair<uint32_t,uint32_t>,3>{{{257,129},{1537,861},{2560,1440}}})
            for(uint32_t mode=0;mode<4;++mode)
            {
                const uint32_t w=(W+1)/2,h=(H+1)/2,gx=(W+127)/128,gy=(H+127)/128;
                std::array<std::shared_ptr<std::vector<uint8_t>>,2> output;
                test.run([&](FramePassContext& fc){
                    auto& g=fc.graph;auto& shaders=fc.shaders;auto view=test.mainView(fc,W,H);
                    const auto cb=fc.frameConstantsFor(view.view);
                    const auto src=g.createTexture({"post input",W,H,1,1,DXGI_FORMAT_R16G16B16A16_FLOAT});
                    const auto grid=g.createTexture({"local grid",gx,gy,32,1,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_DIMENSION_TEXTURE3D});
                    const auto mean=g.createTexture({"local mean",gx,gy,1,1,DXGI_FORMAT_R16_FLOAT});
                    g.addPass("test.input",QueueType::Compute,[&](PassBuilder& b){b.use(src,Use::UavCompute);},[=,&shaders](PassContext& c){
                        const uint32_t k[8]={c.uav(src),W/2+(mode&1),H/2,asU(1000),mode<2?1u:0u,0,0,0};
                        c.cmd->SetPipelineState(shaders.compute("Passes/Shading/Tests/PostImpulse"));c.computeConstants(k,8);c.bindFrameConstants(cb);gpuDispatch(c.cmd,(W+7)/8,(H+7)/8,1);
                    });
                    g.addPass("test.grid",QueueType::Compute,[&](PassBuilder& b){b.use(src,Use::SrvCompute);b.use(grid,Use::UavCompute);b.use(mean,Use::UavCompute);},[=,&shaders](PassContext& c){
                        const uint32_t k[8]={c.srv(src),c.uav(grid),c.uav(mean),0,W,H,0,0};
                        c.cmd->SetPipelineState(shaders.compute("Passes/Shading/LocalExposureGrid"));c.computeConstants(k,8);gpuDispatch(c.cmd,gx,gy,1);
                    });
                    for(uint32_t variant=0;variant<2;++variant)
                    {
                        const auto dst=g.createTexture({"post output",w,h,1,1,DXGI_FORMAT_R16G16B16A16_FLOAT});
                        g.addPass(variant?"test.large":"test.reference",QueueType::Compute,[&](PassBuilder& b){b.use(src,Use::SrvCompute);b.use(grid,Use::SrvCompute);b.use(mean,Use::SrvCompute);b.use(dst,Use::UavCompute);},[=,&shaders](PassContext& c){
                            const uint32_t k[16]={c.srv(src),c.uav(dst),w,h,mode&1?c.srv(grid):UINT32_MAX,c.srv(mean),asU(float(W)/(gx*128)),asU(float(H)/(gy*128)),asU(.8f),asU(.7f),asU(1.1f),asU(.6f),asU(-2.4739312f),0,0,0};
                            c.cmd->SetPipelineState(shaders.compute(variant?"Passes/Shading/PostDownsampleLarge":"Passes/Shading/PostDownsample"));c.computeConstants(k,16);
                            const uint32_t group=variant?16:8;gpuDispatch(c.cmd,(w+group-1)/group,(h+group-1)/group,1);
                        });
                        output[variant]=test.readback(fc,dst);
                    }
                });
                for(uint32_t y=0;y<h;++y)M_CHECK(std::memcmp(output[0]->data()+y*TestFrame::rowPitch(w,8),output[1]->data()+y*TestFrame::rowPitch(w,8),size_t(w)*8)==0,"downsample tile changed pixels %ux%u mode %u row %u",W,H,mode,y);
                pixels+=uint64_t(w)*h;
            }
        logf("PASS post downsample tiles: %llu bit-identical FP16 pixels; HDR ramps and impulses, local exposure on/off, odd edges and 1440p; GBV enabled\n",(unsigned long long)pixels);
        return 0;
    }
    catch(const std::exception& e){logf("FAIL %s\n",e.what());return 1;}
}
