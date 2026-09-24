// P0b gate (ARCHITECTURE_KO.md 7.1-3): 4K empty-frame overhead <= 0.2 ms with a 120-pass graph, harness
// reproducibility within +-2 %. Also measures the same graph without per-pass timestamps (their own cost) and a
// one-pass baseline, at 4K and 1440p.
//
//   unx_gate_empty_frame [--resolution 4K|1440p|both] [--frames 600] [--repeats 5] [--validate] [--out DIR]
//   --validate: debug layer + GPU-based validation for a few frames of the full graph; fails on any error.
#include "EmptyFrameScene.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/Harness.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace unx;
using namespace unx::render;

namespace unx::test
{
int runOverheadExperiments(Device& device, ShaderLibrary& shaders);
}

namespace
{
int validate(const QualityConfig& quality)
{
    DeviceOptions options;
    options.debugLayer = true;
    options.gpuValidation = true;
    Device device(options);
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    Resolution res = resolutionFromString("4K", quality);
    test::EmptyFrameScene scene(device, shaders, res.width, res.height);
    for (bool async : { false, true })
    {
        RenderGraph graph(device);
        graph.setAsyncCompute(async);
        GpuProfiler profiler(device, 2, 1024);
        for (uint64_t f = 0; f < 6; ++f)
        {
            profiler.beginFrame(f);
            scene.build(graph, f);
            graph.execute(&profiler);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) device.queue((QueueType)q).waitCpu(graph.lastFence((QueueType)q));
        }
        const RenderGraphStats& s = graph.stats();
        logf("validate (async %d): %u live passes, %u barriers in %u batches, %u cross-queue syncs, %u command lists, %u transients (%.1f MB aliased of %.1f MB)\n", async, s.livePasses,
             s.barriers, s.barrierBatches, s.crossQueueSyncs, s.commandLists, s.transientResources, s.transientBytesAliased / 1048576.0, s.transientBytesUnaliased / 1048576.0);
    }
    device.waitIdle();
    uint32_t errors = device.drainDebugMessages();
    logf("validate: debug layer + GPU-based validation errors: %u\n", errors);
    return errors == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string resolutions = "both", out, qualityPath = std::string(UNX_SOURCE_DIR) + "/Config/quality";
        uint32_t frames = 600, repeats = 5;
        bool doValidate = false, doExperiments = false;
        D3D12_COMMAND_QUEUE_PRIORITY priority = DeviceOptions{}.queuePriority;
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--resolution") resolutions = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--repeats") repeats = (uint32_t)std::stoul(next());
            else if (a == "--validate") doValidate = true;
            else if (a == "--experiments") doExperiments = true;
            else if (a == "--queue-priority") priority = next() == "high" ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            else if (a == "--out") out = next();
            else if (a == "--quality") qualityPath = next();
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(qualityPath);
        if (doValidate) return validate(quality);
        if (out.empty()) out = std::string(UNX_SOURCE_DIR) + "/Results/Gates/EmptyFrame";

        DeviceOptions deviceOptions;
        deviceOptions.queuePriority = priority;
        Device device(deviceOptions);
        logf("queue priority %s\n", priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "HIGH" : "NORMAL");
        const DeviceCaps& caps = device.caps();
        logf("adapter %s, driver %s, D3D12Core %s (%s), NVAPI %s branch %s, OMM %d, SER %d, quality %s\n", caps.adapter.c_str(), caps.driver.c_str(), caps.runtimeVersion.c_str(),
             caps.runtimePath.c_str(), caps.nvapiInterface.c_str(), caps.nvapiBranch.c_str(), caps.nvapiOpacityMicromap, caps.nvapiThreadReordering, quality.shortHash().c_str());
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        if (doExperiments) return test::runOverheadExperiments(device, shaders);
        Harness harness(device, quality);

        std::vector<std::string> list = resolutions == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutions };
        bool pass = true;
        for (const std::string& name : list)
        {
            Resolution res = resolutionFromString(name, quality);
            test::EmptyFrameScene scene(device, shaders, res.width, res.height);
            HarnessOptions opt;
            opt.frames = frames;
            opt.outputDirectory = out;

            std::vector<double> medians;
            for (uint32_t k = 0; k < repeats; ++k)
            {
                opt.label = format("empty_frame_120_run%u", k);
                opt.writePassCsv = k == 0;
                HarnessResult r = harness.run(res, opt, [&](RenderGraph& g, const Resolution&, uint64_t f) { scene.build(g, f); });
                harness.printSummary(r);
                medians.push_back(r.gpuFrameMs.median);
            }
            opt.label = "empty_frame_120_no_pass_timestamps";
            opt.passTimestamps = false;
            HarnessResult noTs = harness.run(res, opt, [&](RenderGraph& g, const Resolution&, uint64_t f) { scene.build(g, f); });
            harness.printSummary(noTs);
            opt.label = "empty_frame_120_async_compute";
            opt.passTimestamps = true;
            opt.asyncCompute = true;
            HarnessResult async = harness.run(res, opt, [&](RenderGraph& g, const Resolution&, uint64_t f) { scene.build(g, f); });
            harness.printSummary(async);
            opt.asyncCompute = false;
            opt.label = "baseline_1_pass";
            HarnessResult base = harness.run(res, opt, [&](RenderGraph& g, const Resolution&, uint64_t f) { scene.build(g, f, false); });
            harness.printSummary(base);

            std::vector<double> sorted = medians;
            std::sort(sorted.begin(), sorted.end());
            const double center = sorted[sorted.size() / 2];
            double spread = 0;
            for (double m : medians) spread = std::max(spread, std::fabs(m - center) / center);
            const bool overheadOk = res.name != "4K" || center <= 0.2;
            const bool reproOk = spread <= 0.02;
            logf("GATE %s: 120-pass frame median of %u runs %.4f ms (limit 0.2 ms at 4K) -> %s; run-to-run spread %.2f %% (limit 2 %%) -> %s; "
                 "without per-pass timestamps %.4f ms; with in-frame async compute %.4f ms; 1-pass baseline %.4f ms\n",
                 res.name.c_str(), repeats, center, overheadOk ? "PASS" : "FAIL", spread * 100, reproOk ? "PASS" : "FAIL", noTs.gpuFrameMs.median, async.gpuFrameMs.median,
                 base.gpuFrameMs.median);
            pass = pass && overheadOk && reproOk;
        }
        for (const std::string& k : quality.unreadKeys()) logf("note: quality key %s was not read by this program\n", k.c_str());
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
