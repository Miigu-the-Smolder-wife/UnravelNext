#include "../../Material/Tests/MTestFrame.h"
#include "../TsrRejectPolicy.h"
#include "unx/render/GpuLock.h"
#include <array>
#include <bit>
#include <cstdio>
#include <fstream>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
int main(int argc, char** argv)
{
    try
    {
        bool timing = false, quick = false;
        uint32_t requestedWidth = 0, requestedHeight = 0;
        std::string evidence;
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--timing") == 0) timing = true;
            else if (std::strcmp(argv[i], "--quick") == 0) quick = true;
            else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) requestedWidth = (uint32_t)std::stoul(argv[++i]);
            else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) requestedHeight = (uint32_t)std::stoul(argv[++i]);
            else if (std::strcmp(argv[i], "--evidence") == 0 && i + 1 < argc) evidence = argv[++i];
            else fail("unknown argument %s", argv[i]);
        }
        if ((requestedWidth == 0) != (requestedHeight == 0) || requestedWidth > 16366 || requestedHeight > 16366)
            fail("width and height must both be supplied and fit the overscan");
        if (timing) requireGpuLock("TSR combined flicker/rejection A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back(); test.setScene(scene);
        std::vector<std::pair<uint32_t, uint32_t>> sizes = {{1,1}, {15,17}, {16,16}, {17,31}, {257,35}, {1280,720}, {1920,1080}};
        if (requestedWidth) sizes = {{requestedWidth, requestedHeight}};
        else if (timing) sizes = {{1280,720}};
        struct Pair { uint32_t w,h,options,order; double reference,candidate; };
        std::vector<Pair> pairs;
        uint64_t pixels = 0;
        for (const auto [w,h] : sizes)
        {
            std::vector<double> ratios;
            const uint32_t warmup = 4, measured = quick ? 16u : 64u;
            const uint32_t cases = timing ? warmup + measured : (w >= 1280 ? (quick ? 2u : 16u) : 32u);
            for (uint32_t seed = 0; seed < cases; ++seed)
            {
                // Flicker is present by definition. Remaining options vary independently
                // for correctness; timed none/all pairs exercise both production policies.
                const uint32_t options = timing ? (((seed / 2) & 1u) ? 15u : 1u) : w >= 1280 && quick ? (seed ? 15u : 1u) : 1u | ((seed >> 1) & 7u) * 2;
                const bool baselinePrefix = shading::detail::useFusedTsrRejection(w, h, options);
                test.frame.deltaTime = (seed & 1u) ? 1.0f / 30 : 1.0f / 60;
                std::array<std::shared_ptr<std::vector<uint8_t>>, 10> results;
                test.run([&](FramePassContext& fc) {
                    const ViewResources view = test.mainView(fc, w, h);
                    const DXGI_FORMAT formats[10] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                        DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
                    std::array<TextureRef,10> in;
                    for (uint32_t i = 0; i < 10; ++i) in[i] = fc.graph.createTexture({"combined input",w,h,1,1,formats[i]});
                    fc.graph.addPass("combined input",QueueType::Graphics,
                        [&](PassBuilder& b) { for (auto t:in) b.use(t,Use::UavCompute); },
                        [&,in,seed](PassContext& c) {
                            uint32_t k[16] = {};
                            for (uint32_t i = 0; i < 8; ++i) k[i] = c.uav(in[i]);
                            k[8]=w; k[9]=h; k[10]=seed; k[12]=c.uav(in[8]); k[13]=c.uav(in[9]);
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TsrCombinedInput"));
                            c.computeConstants(k,16); c.cmd->Dispatch((w+7)/8,(h+7)/8,1);
                        });
                    for (uint32_t slot=0;slot<2;++slot)
                    {
                        const uint32_t variant=slot^(seed&1u);
                        const std::string label=variant ? "candidate " : "reference ";
                        std::array<TextureRef,5> out = {
                            fc.graph.createTexture({"reject",w,h,1,1,DXGI_FORMAT_R8G8B8A8_UNORM}),
                            fc.graph.createTexture({"guide",w,h,1,1,DXGI_FORMAT_R10G10B10A2_UNORM}),
                            fc.graph.createTexture({"AA",w,h,1,1,DXGI_FORMAT_R8G8_UNORM}),
                            fc.graph.createTexture({"moire",w,h,1,1,DXGI_FORMAT_R16_FLOAT}),
                            fc.graph.createTexture({"flicker history",w,h,1,1,DXGI_FORMAT_R8G8B8A8_UNORM})};
                        TextureRef rgb, luma;
                        if (!variant)
                            fc.graph.addPass(label+"flicker",QueueType::Graphics,
                                [&](PassBuilder& b) { for(auto t:in) b.use(t,Use::SrvCompute); b.use(out[3],Use::UavCompute); b.use(out[4],Use::UavCompute); },
                                [&,in,out,view,seed,options](PassContext& c) {
                                    uint32_t k[12] = {c.srv(in[0]),c.srv(in[8]),c.srv(in[2]),c.srv(in[9]),c.uav(out[3]),c.uav(out[4]),w,h,
                                        seed&1u,(options&2u)?c.srv(in[4]):UINT32_MAX,0,0};
                                    c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/TsrFlicker"));
                                    c.bindFrameConstants(view.frameConstants); c.computeConstants(k,12); c.cmd->Dispatch((w+15)/16,(h+15)/16,1);
                                });
                        if (variant || baselinePrefix)
                        {
                            rgb=fc.graph.createTexture({"RGB prefix",w+10,h+10,1,1,DXGI_FORMAT_R32G32B32A32_UINT});
                            if(variant) luma=fc.graph.createTexture({"luma prefix",w+18,h+18,1,1,DXGI_FORMAT_R32G32B32A32_UINT});
                            fc.graph.addPass(label+"prefix",QueueType::Graphics,
                                [&](PassBuilder& b) { b.use(in[0],Use::SrvCompute); b.use(in[1],Use::SrvCompute); b.use(rgb,Use::UavCompute);
                                    if(variant) { b.use(in[8],Use::SrvCompute); b.use(luma,Use::UavCompute); } },
                                [&,in,rgb,luma,variant](PassContext& c) {
                                    const uint32_t k[8] = {c.srv(in[0]),c.srv(in[1]),c.uav(rgb),w,h,variant?c.srv(in[8]):0,variant?c.uav(luma):0,0};
                                    c.cmd->SetPipelineState(fc.shaders.compute(variant ? "Passes/Shading/Tests/TsrCombinedPrefix" : "Passes/Shading/TsrRejectPrefix"));
                                    c.computeConstants(k,8); c.cmd->Dispatch((w+(variant?33:25))/16,(h+(variant?33:25))/16,1);
                                });
                        }
                        fc.graph.addPass(label+"tail",QueueType::Graphics,
                            [&](PassBuilder& b) { for(auto t:in) b.use(t,Use::SrvCompute);
                                for(uint32_t i=0;i<3;++i) b.use(out[i],Use::UavCompute);
                                if(variant) { b.use(luma,Use::SrvCompute); b.use(out[3],Use::UavCompute); b.use(out[4],Use::UavCompute); }
                                else b.use(out[3],Use::SrvCompute);
                                if(variant||baselinePrefix) b.use(rgb,Use::SrvCompute); b.keep(); },
                            [&,in,out,rgb,luma,view,variant,seed,options,baselinePrefix](PassContext& c) {
                                uint32_t k[24] = {c.srv(in[0]),c.srv(in[1]),c.srv(in[2]),c.uav(out[0]),c.uav(out[1]),c.uav(out[2]),w,h,
                                    std::bit_cast<uint32_t>(0.03f),variant?0u:c.srv(out[3]),(options&2u)?c.srv(in[4]):UINT32_MAX,
                                    (options&4u)?c.srv(in[5]):UINT32_MAX,(options&8u)?c.srv(in[6]):UINT32_MAX,(options&8u)?c.srv(in[7]):UINT32_MAX,
                                    (variant||baselinePrefix)?c.srv(rgb):0u,0};
                                if(variant) { k[16]=c.srv(luma); k[17]=c.srv(in[8]); k[18]=c.srv(in[9]); k[19]=seed&1u; k[20]=c.uav(out[3]); k[21]=c.uav(out[4]); }
                                c.cmd->SetPipelineState(fc.shaders.compute(variant ? "Passes/Shading/Tests/TsrCombinedTail" : baselinePrefix ? "Passes/Shading/TsrRejectFromPrefix" : "Passes/Shading/TsrReject"));
                                c.bindFrameConstants(view.frameConstants); c.computeConstants(k,24); c.cmd->Dispatch((w+15)/16,(h+15)/16,1);
                            });
                        if(!timing) for(uint32_t i=0;i<5;++i) results[variant*5+i]=test.readback(fc,out[i]);
                    }
                });
                if(timing)
                {
                    if(seed>=warmup)
                    {
                        double sums[2]={};
                        for(const auto& pass:test.lastTiming.passes)
                            if(pass.name.starts_with("reference ")) sums[0]+=pass.durationMs();
                            else if(pass.name.starts_with("candidate ")) sums[1]+=pass.durationMs();
                        M_CHECK(sums[0]>0&&sums[1]>0,"missing pipeline timestamps");
                        pairs.push_back({w,h,options,seed&1u,sums[0],sums[1]}); ratios.push_back(sums[1]/sums[0]);
                    }
                }
                else
                {
                    pixels+=uint64_t(w)*h;
                    for(uint32_t i=0;i<5;++i)
                    {
                        const uint32_t bytes=(i==2||i==3)?2:4,pitch=TestFrame::rowPitch(w,bytes);
                        for(uint32_t y=0;y<h;++y)
                            M_CHECK(std::memcmp(results[i]->data()+y*pitch,results[i+5]->data()+y*pitch,w*bytes)==0,
                                "combined %ux%u seed %u options %u output %u row %u differs",w,h,seed,options,i,y);
                    }
                }
            }
            if(timing)
            {
                std::sort(ratios.begin(),ratios.end());
                logf("TSR combined %ux%u paired candidate/current-production median %.5f range [%.5f,%.5f] wins %zu/%zu; both orders and all pipeline passes\n",
                    w,h,ratios[ratios.size()/2],ratios.front(),ratios.back(),size_t(std::count_if(ratios.begin(),ratios.end(),[](double v){return v<1;})),ratios.size());
            }
        }
        if(timing&&!evidence.empty())
        {
            std::ofstream out(evidence,std::ios::binary); if(!out) fail("cannot write evidence"); out.precision(12);
            out<<"{\"prototype\":\"combined-flicker-rejection\",\"cleanGpuAcceptance\":false,\"qualityHash\":\""<<test.quality.hash()<<"\",\"pairs\":[";
            for(size_t i=0;i<pairs.size();++i) { const auto& p=pairs[i]; if(i)out<<',';
                out<<"{\"width\":"<<p.w<<",\"height\":"<<p.h<<",\"options\":"<<p.options<<",\"candidateFirst\":"<<(p.order?"true":"false")
                   <<",\"referenceMs\":"<<p.reference<<",\"candidateMs\":"<<p.candidate<<",\"ratio\":"<<p.candidate/p.reference<<'}'; }
            out<<"]}\n";
        }
        if(!timing) logf("PASS combined TSR: %llu pixels, all five outputs byte-identical to current production; GBV enabled\n",(unsigned long long)pixels);
        return 0;
    }
    catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}
