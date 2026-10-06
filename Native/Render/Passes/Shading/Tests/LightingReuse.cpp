#include "../../Material/Tests/MTestFrame.h"
#include <array>
#include <bit>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true,true);
        scene::Scene scene;scene.cameras.emplace_back();scene.cameras[0].position={0,0,5};scene.cameras[0].forward={0,0,-1};
        scene.sun.direction=normalize(float3{0.2f,0.5f,1});scene.sun.illuminance=120;
        for(uint32_t i=0;i<4;++i){scene::Material m;if(i==1){m.cls=scene::MaterialClass::Foliage;m.transmission=0.4f;}scene.materials.push_back(m);}
        test.setScene(scene);
        uint64_t temporalPixels=0,combinedPixels=0;uint32_t maximumRadianceUlps=0;
        for(const auto [w,h]:std::array<std::pair<uint32_t,uint32_t>,4>{{{1,1},{17,31},{257,129},{1280,720}}})
        for(uint32_t seed=0;seed<(w==1280?4u:8u);++seed)
        for(uint32_t mode=0;mode<2;++mode)
        {
            std::array<std::shared_ptr<std::vector<uint8_t>>,12> results;
            test.run([&](FramePassContext& fc){
                const auto view=test.mainView(fc,w,h);
                const DXGI_FORMAT formats[14]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_UINT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8_UINT,
                    DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
                std::array<TextureRef,14> input;
                for(uint32_t i=0;i<14;++i)input[i]=fc.graph.createTexture({"lighting reuse input",w,h,1,1,formats[i]});
                fc.graph.addPass("lighting inputs",QueueType::Compute,[&](PassBuilder& b){for(auto t:input)b.use(t,Use::UavCompute);},
                    [&,input,view,seed,mode](PassContext& c){uint32_t k[20]{};for(uint32_t i=0;i<14;++i)k[i]=c.uav(input[i]);k[16]=w;k[17]=h;k[18]=seed;k[19]=mode;
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/LightingReusePattern"));c.bindFrameConstants(view.frameConstants);c.computeConstants(k,20);c.cmd->Dispatch((w+7)/8,(h+7)/8,1);});
                BufferRef tiles,coverageTiles;
                ComPtr<ID3D12Resource> tileData;
                if(mode){
                    std::vector<uint32_t> indices;for(uint32_t y=0;y<(h+7)/8;++y)for(uint32_t x=0;x<(w+7)/8;++x)indices.push_back(x|(y<<16));
                    tileData=uploadStatic(fc.device,indices.data(),indices.size()*4,L"combined shade tiles");test.keep(tileData);
                    tiles=fc.graph.importBuffer(tileData.Get(),{"combined shade tiles",indices.size()*4,0});
                }else{
                    // V coverage headers: zero records in the pattern's empty 8x8 tiles.
                    std::vector<uint32_t> records(((w+7)/8)*((h+7)/8)*8,0);
                    for(uint32_t y=0;y<(h+7)/8;++y)for(uint32_t x=0;x<(w+7)/8;++x)records[(y*((w+7)/8)+x)*8]=((x+y+seed)%5u)==0?0u:1u;
                    tileData=uploadStatic(fc.device,records.data(),records.size()*4,L"temporal coverage tiles");test.keep(tileData);
                    coverageTiles=fc.graph.importBuffer(tileData.Get(),{"temporal coverage tiles",records.size()*4,0});
                }
                for(uint32_t variant=0;variant<2;++variant)
                {
                    const DXGI_FORMAT outFormats[6]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8_UINT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R8G8_UNORM};
                    std::array<TextureRef,6> output;
                    for(uint32_t i=0;i<(mode?1u:6u);++i)output[i]=fc.graph.createTexture({"lighting reuse output",w,h,1,1,mode?DXGI_FORMAT_R32G32B32A32_FLOAT:outFormats[i]});
                    const auto direct=mode?fc.graph.createTexture({"split direct",w,h,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT}):TextureRef{};
                    for(uint32_t stage=0;stage<(mode&&!variant?2u:1u);++stage)
                    fc.graph.addPass("lighting compare",QueueType::Compute,[&](PassBuilder& b){for(auto t:input)b.use(t,Use::SrvCompute);for(auto t:output)if(t.valid())b.use(t,Use::UavCompute);
                        if(mode){b.use(direct,Use::UavCompute);b.use(tiles,Use::SrvCompute);}else b.use(coverageTiles,Use::SrvCompute);b.keep();},
                        [&,input,output,direct,tiles,coverageTiles,view,seed,mode,variant,stage](PassContext& c){
                            auto bits=[](float v){return std::bit_cast<uint32_t>(v);};uint32_t k[48];std::fill(std::begin(k),std::end(k),gpu::kNone);
                            if(!mode){
                                k[0]=c.srv(input[0]);k[1]=c.srv(input[1]);k[2]=c.srv(input[2]);k[3]=c.srv(input[3]);k[4]=w;k[5]=h;k[6]=seed&1u;k[7]=bits(seed&2u?0.5f:2.0f);
                                for(uint32_t i=0;i<4;++i)k[8+i]=c.srv(input[4+i]);k[12]=c.srv(input[8]);k[15]=c.srv(input[9]);
                                for(uint32_t i=0;i<6;++i)k[16+i]=c.uav(output[i]);k[24]=bits(16);k[25]=bits(2);k[26]=bits(0.05f);k[27]=bits(2.5f);
                                k[22]=(seed&2u)?c.srv(coverageTiles):gpu::kNone;
                                c.cmd->SetPipelineState(fc.shaders.compute(variant?"Passes/Shading/MegaLightsTemporal":"Passes/Shading/Tests/MegaLightsTemporalReference"));
                            }else{
                                k[0]=c.srv(input[3]);k[1]=c.srv(input[2]);k[2]=c.srv(input[9]);k[3]=c.uav(output[0]);k[4]=c.srv(tiles);k[5]=0;k[6]=(uint32_t)material::ShadeClass::Opaque;
                                k[7]=c.srv(input[13]);k[18]=4096;k[39]=c.srv(input[10]);k[40]=c.uav(direct);k[41]=c.srv(input[0]);k[45]=c.srv(input[11]);
                                if(variant||stage){k[46]=(seed&2u)?c.srv(input[12]):gpu::kNone;k[47]=c.srv(input[10]);}
                                const std::string name=variant?"Passes/Shading/ShadeCombined.AREA1.PLANAR0.OUTPUT1":stage?"Passes/Shading/ShadeIndirect.OUTPUT1.FALLBACK0.PLANAR0.LAYERED0":"Passes/Shading/ShadeOpaque.FALLBACK0.AREA1.PLANAR0.LAYERED0";
                                c.cmd->SetPipelineState(fc.shaders.compute(name));
                            }
                            c.bindFrameConstants(view.frameConstants);c.computeConstants(k,mode?48:28);
                            if(mode)c.cmd->Dispatch(((w+7)/8)*((h+7)/8),1,1);else c.cmd->Dispatch((w+7)/8,(h+7)/8,1);
                        });
                    for(uint32_t i=0;i<(mode?1u:6u);++i)results[variant*6+i]=test.readback(fc,output[i]);
                }
            });
            const uint32_t bytes[6]={8,8,8,1,4,2};
            for(uint32_t i=0;i<(mode?1u:6u);++i){const uint32_t b=mode?16:bytes[i],pitch=TestFrame::rowPitch(w,b);
                for(uint32_t y=0;y<h;++y){
                    const auto* a=results[i]->data()+y*pitch;const auto* z=results[6+i]->data()+y*pitch;
                    if(mode){
                        // The fused float32 expression may contract arithmetic across
                        // the former UAV boundary. Bound that rounding before any f16
                        // output quantization; this is not a tolerance on missing light.
                        for(uint32_t x=0;x<w*4;++x){float av,bv;uint32_t ai,bi;std::memcpy(&av,a+x*4,4);std::memcpy(&bv,z+x*4,4);std::memcpy(&ai,&av,4);std::memcpy(&bi,&bv,4);
                            const uint32_t ulps=ai>bi?ai-bi:bi-ai;maximumRadianceUlps=std::max(maximumRadianceUlps,ulps);
                            M_CHECK(std::isfinite(av)&&std::isfinite(bv)&&av>=0&&bv>=0&&ulps<=8,"combined radiance %ux%u seed %u component %u,%u: %.9g vs %.9g (%u ULP)",w,h,seed,x,y,av,bv,ulps);
                        }
                    }else M_CHECK(std::memcmp(a,z,w*b)==0,"temporal %ux%u, seed %u, output %u, row %u differs",w,h,seed,i,y);
                }}
            (mode?combinedPixels:temporalPixels)+=uint64_t(w)*h;
        }
        logf("PASS lighting reuse: %llu temporal pixels (six outputs) bit-identical; %llu combined shading pixels, float32 radiance worst %u ULP before f16 output; reset/history, holes, depth rejection, partial groups, foliage, GI/AO; GBV enabled\n",(unsigned long long)temporalPixels,(unsigned long long)combinedPixels,maximumRadianceUlps);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
