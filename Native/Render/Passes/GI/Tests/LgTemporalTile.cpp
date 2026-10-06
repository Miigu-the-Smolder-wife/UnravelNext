#include "../Lumen/LgTemporalTile.h"
#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include <array>
#include <bit>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
int main(int argc, char** argv)
{
    try
    {
        bool timing = argc == 2 && std::strcmp(argv[1], "--timing") == 0;
        if (timing) requireGpuLock("GI temporal tile A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 32); if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.cameras.emplace_back(); scene.cameras[0].position = {0,0,5}; scene.cameras[0].forward = {0,0,-1};
        scene.materials.emplace_back(); scene.materials.emplace_back(); scene.materials[1].cls = scene::MaterialClass::Foliage;
        test.setScene(scene); uint64_t checked = 0;
        for (auto [w,h] : std::array<std::pair<uint32_t,uint32_t>,4>{{{1,1},{17,31},{257,129},{1280,720}}})
        {
            if (timing && w != 1280) continue;
            for (uint32_t mode = 0; mode < 4; ++mode)
            {
                std::array<std::vector<double>,2> times;
                for (uint32_t seed = 0; seed < (timing ? 100u : 4u); ++seed)
                {
                    std::array<std::shared_ptr<std::vector<uint8_t>>,8> results;
                    test.run([&](FramePassContext& fc) {
                        auto view = test.mainView(fc,w,h);
                        const DXGI_FORMAT formats[10] = {DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_UINT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32_UINT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_UINT};
                        std::array<TextureRef,10> input;
                        for (uint32_t i=0;i<10;++i) input[i]=fc.graph.createTexture({"GI temporal input",w,h,1,1,formats[i]});
                        fc.graph.addPass("GI input",QueueType::Graphics,[&](PassBuilder& b){for(auto t:input)b.use(t,Use::UavCompute);},
                            [&,input,view,seed,mode](PassContext& c){uint32_t k[16]{};for(uint32_t i=0;i<10;++i)k[i]=c.uav(input[i]);k[12]=w;k[13]=h;k[14]=seed;k[15]=mode;
                                c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/Tests/LgTemporalInput"));c.bindFrameConstants(view.frameConstants);c.computeConstants(k,16);c.cmd->Dispatch((w+7)/8,(h+7)/8,1);});
                        for(uint32_t slot=0;slot<2;++slot)
                        {
                            const uint32_t variant=slot^(seed&1u);
                            std::array<TextureRef,4> output;
                            for(uint32_t i=0;i<4;++i)output[i]=fc.graph.createTexture({"GI temporal output",w,h,1,1,i==2?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R16G16B16A16_FLOAT});
                            fc.graph.addPass(variant?"GI optimized":"GI reference",QueueType::Graphics,
                                [&](PassBuilder& b){for(auto t:input)b.use(t,Use::SrvCompute);for(auto t:output)b.use(t,Use::UavCompute);b.keep();},
                                [&,input,output,variant,view,mode,seed](PassContext& c){
                                    auto bits=[](float f){return std::bit_cast<uint32_t>(f);};uint32_t k[48]{};
                                    k[0]=c.srv(input[0]);k[1]=c.srv(input[1]);k[2]=k[3]=gpu::kNone;
                                    k[4]=c.srv(input[2]);k[5]=c.srv(input[3]);k[6]=c.uav(output[0]);k[7]=c.uav(output[1]);k[8]=c.uav(output[2]);
                                    k[9]=c.srv(input[5]);k[10]=c.srv(input[6]);k[11]=c.srv(input[7]);k[12]=bits(10);k[13]=bits(.005f);k[14]=bits(.1f);k[15]=bits(.9f);
                                    std::memcpy(k+16,&view.view.invViewProj,64);k[32]=w;k[33]=h;k[38]=(mode?0x10000u:0u)|((seed&2u)?180u<<24:0u);
                                    k[42]=c.srv(input[4]);k[43]=c.srv(input[8]);k[44]=c.uav(output[3]);k[45]=c.srv(input[9]);k[46]=bits(seed&1u?2.0f:.5f);k[47]=bits(.03f);
                                    c.cmd->SetPipelineState(fc.shaders.compute(variant?"Passes/GI/Lumen/LgTemporal":"Passes/GI/Tests/LgTemporalReference"));
                                    c.computeConstants(k,48);c.bindFrameConstants(view.frameConstants); const uint32_t tile=variant?LG_TEMPORAL_TILE:8u; c.cmd->Dispatch((w+tile-1)/tile,(h+tile-1)/tile,1);
                                });
                            if(!timing)for(uint32_t i=0;i<4;++i)results[variant*4+i]=test.readback(fc,output[i]);
                        }
                    });
                    if(timing)
                    {
                        if(seed>=20)for(const auto& p:test.lastTiming.passes)
                            if(p.name=="GI reference"||p.name=="GI optimized")times[p.name=="GI optimized"].push_back(p.durationMs());
                    }
                    else
                    {
                        for(uint32_t i=0;i<4;++i)for(uint32_t y=0;y<h;++y)
                            M_CHECK(std::memcmp(results[i]->data()+y*TestFrame::rowPitch(w,8),results[i+4]->data()+y*TestFrame::rowPitch(w,8),w*8)==0,
                                    "GI temporal differs width %u mode %u seed %u output %u row %u",w,mode,seed,i,y);
                        checked+=uint64_t(w)*h;
                    }
                }
                if(timing){for(auto& t:times){M_CHECK(t.size()==80,"missing GPU timestamps");std::sort(t.begin(),t.end());}
                    logf("GI temporal mode %u reference %.6f optimized %.6f ms reduction %.2f%% (80 alternating pairs)\n",mode,times[0][40],times[1][40],100*(1-times[1][40]/times[0][40]));}
            }
        }
        if(!timing)logf("PASS GI temporal tile: %llu pixels, four outputs bit-identical; D3D12 GPU validation enabled\n",(unsigned long long)checked);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
