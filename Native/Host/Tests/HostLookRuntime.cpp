#include "Renderer/HostRenderer.h"
#include "TestScenes.h"
#include "unx/core/File.h"
#include <cmath>
#include <limits>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

int main()
{
    try
    {
        HostRendererOptions options;
        options.debugLayer = true;
        options.shaderDirectory = executableDirectory() / "shaders";
        options.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        options.qualityOverrides = {"output.dynamic_resolution_target_ms=0.0"};
        HostRenderer host(options); host.scene() = test::oneBox(); host.commit();
        auto camera = host.scene().cameras[0]; camera.ev100 = 14;
        constexpr uint32_t W=640,H=360;
        std::vector<uint32_t> before(W*H),after(W*H);
        uint64_t frame=0;
        auto render=[&](std::vector<uint32_t>* pixels=nullptr){
            FramePacket packet;packet.frameIndex=frame++;packet.time=double(frame)/60;packet.deltaTime=1.0f/60;
            packet.width=W;packet.height=H;packet.camera=camera;
            host.renderStandalone(host.queueFrame(std::move(packet)),pixels?pixels->data():nullptr,pixels?pixels->size()*4:0);
            if(host.lastFrameFlags().first!=0)fail("look edit injected a camera discontinuity");
        };
        PostExtendedSettings extended;extended.enabled=true;extended.localExposure=0;
        host.setPostExtended(extended);
        for(int i=0;i<7;++i)render(i==6?&before:nullptr);
        const uint32_t revision=host.latestStats().graph.sceneRevision;
        auto rejects=[&](auto mutate){auto bad=extended;mutate(bad);bool rejected=false;try{host.setPostExtended(bad);}catch(const std::exception&){rejected=true;}
            if(!rejected)fail("invalid live look was accepted");};
        rejects([](auto& p){p.grain=std::numeric_limits<float>::quiet_NaN();});
        rejects([](auto& p){p.toneCurve=2;});rejects([](auto& p){p.gradingLutSize=65;});
        rejects([](auto& p){p.renderScale=0;});rejects([](auto& p){p.motionSamples=6;});
        rejects([](auto& p){p.fringeStart=1;});rejects([](auto& p){p.exposureBrighterSeconds=0;});
        ColorGradingDesc grading;grading.enabled=true;grading.temperature=4500;grading.global.gain[3]=0.6f;
        host.setColorGrading(grading);
        PostSettingsDesc legacy;legacy.bloomStrength=0.2f;legacy.vignette=0.75f;legacy.motionBlurShutter=0;
        host.setPost(legacy,0);
        extended.toneCurve=1;extended.grain=0.02f;extended.sharpen=0.3f;extended.localExposure=1;
        extended.localHighlight=0.7f;extended.localShadow=0.6f;extended.localDetail=1.1f;extended.localBlend=0.5f;
        extended.fringe=0.25f;extended.fringeStart=0.6f;extended.lensFlare=1;extended.flareThreshold=0.2f;
        extended.flareIntensity=0.1f;extended.flareBokehSize=2;extended.flareTintB=0.5f;
        extended.exposureBrighterSeconds=0.4f;extended.exposureDarkerSeconds=1.3f;
        host.setPostExtended(extended);
        for(int i=0;i<7;++i)render(i==6?&after:nullptr);
        uint64_t changed=0;
        for(size_t i=0;i<before.size();++i)changed+=before[i]!=after[i];
        if(changed<W*H/10)fail("live look had no meaningful visible effect");
        if(host.latestStats().graph.sceneRevision!=revision)fail("look edit changed scene revision");
        // Exercise prepared LUT replacement and output history while the same
        // renderer, scene and simulation consumer stay alive.
        for(int i=0;i<6;++i)
        {
            extended.gradingLutSize=i%2?64:16;extended.bloomLevels=i%2?6:4;
            extended.renderScale=i%2?0.75f:0.5f;extended.tsrHistoryPercent=i%2?150.0f:100.0f;
            extended.paniniD=i==3?0.2f:0;
            host.setPostExtended(extended);host.setLens(i%2?0.02f:0,3);
            for(int f=0;f<3;++f)render();
            const auto pacing=host.framePacing();
            if(pacing.outputWidth!=W||pacing.outputHeight!=H||pacing.renderHeight!=uint32_t(std::lround(H*extended.renderScale)))
                fail("live resolution did not reach the actual frame");
        }
        extended.renderScale=1;extended.tsrHistoryPercent=100;extended.paniniD=0.2f;host.setPostExtended(extended);host.setLens(0,3);
        for(int i=0;i<3;++i)render();
        extended.paniniD=0;host.setPostExtended(extended);
        for(int i=0;i<3;++i)render();
        if(host.framePacing().renderWidth!=W || host.latestStats().graph.sceneRevision!=revision)fail("resource edits did not preserve scene and restore native size");
        if(host.debugErrors())fail("live look D3D12 errors");
        logf("PASS live look: %llu changed pixels; seven invalid snapshots refused; grading/tone/bloom/local exposure/grain/lens consumer active; six LUT/resolution replacements and native restore; scene revision unchanged; no injected discontinuities; D3D12 errors 0\n",(unsigned long long)changed);
        return 0;
    }
    catch(const std::exception& e){logf("FAIL %s\n",e.what());return 1;}
}
