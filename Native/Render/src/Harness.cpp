#include "unx/render/Harness.h"

#include "unx/BuildIdentity.generated.h"
#include "unx/core/File.h"
#include "unx/render/GpuLock.h"

#include <nvapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sstream>

namespace unx::render
{
namespace
{
double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string jsonString(const std::string& s)
{
    std::string o = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}

std::string jsonDistribution(const Distribution& d)
{
    return format("{\"count\": %u, \"median\": %.6f, \"p95\": %.6f, \"p99\": %.6f, \"mean\": %.6f, \"min\": %.6f, \"max\": %.6f}", d.count, d.median, d.p95, d.p99, d.mean, d.min, d.max);
}

std::string stamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}
} // namespace

Distribution Distribution::of(std::vector<double> v)
{
    Distribution d;
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    auto pct = [&](double q) { size_t i = (size_t)std::ceil(q * (double)v.size()); return v[std::min(v.size() - 1, i ? i - 1 : 0)]; };
    d.count = (uint32_t)v.size();
    d.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    d.p95 = pct(0.95);
    d.p99 = pct(0.99);
    double sum = 0;
    for (double x : v) sum += x;
    d.mean = sum / (double)v.size();
    d.min = v.front();
    d.max = v.back();
    return d;
}

Resolution resolutionFromString(const std::string& text, const QualityConfig& quality)
{
    Resolution r;
    if (text == "4K" || text == "3840x2160") r = { 3840, 2160, "4K" };
    else if (text == "1440p" || text == "2560x1440") r = { 2560, 1440, "1440p" };
    else fail("resolution '%s' refused: measurements are taken only at 4K (3840x2160) and 1440p (2560x1440)", text.c_str());
    const std::string key = format("%ux%u", r.width, r.height);
    auto allowed = quality.strings("output.resolutions");
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) fail("resolution %s is not listed in output.resolutions of %s", key.c_str(), quality.origin().c_str());
    return r;
}

Harness::Harness(Device& device, const QualityConfig& quality) : m_device(device), m_quality(quality)
{
    if (device.caps().nvapi)
    {
        NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
        NvU32 count = 0;
        if (NvAPI_EnumPhysicalGPUs(handles, &count) == NVAPI_OK && count > 0) m_gpu = handles[0];
    }
}

Harness::~Harness() = default;

uint32_t Harness::sampleSmClockMHz()
{
    if (!m_gpu) return 0;
    NV_GPU_CLOCK_FREQUENCIES f{};
    f.version = NV_GPU_CLOCK_FREQUENCIES_VER;
    f.ClockType = NV_GPU_CLOCK_FREQUENCIES_CURRENT_FREQ;
    if (NvAPI_GPU_GetAllClockFrequencies(static_cast<NvPhysicalGpuHandle>(m_gpu), &f) != NVAPI_OK) return 0;
    const auto& g = f.domain[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS];
    return g.bIsPresent ? g.frequency / 1000 : 0;
}

HarnessResult Harness::run(const Resolution& resolution, const HarnessOptions& options, const BuildFrame& build)
{
    if (resolution.width == 0) fail("harness: resolution not set");
    const std::string lockHolder = requireGpuLock("Harness::run");
    RenderGraph graph(m_device);
    graph.setAsyncCompute(options.asyncCompute);
    GpuProfiler profiler(m_device, options.framesInFlight, 1024);
    profiler.setPassTimestamps(options.passTimestamps);

    struct CpuFrame
    {
        double total, record, submit;
    };
    std::vector<std::array<uint64_t, kQueueTypeCount>> slotFence(options.framesInFlight, std::array<uint64_t, kQueueTypeCount>{});
    std::vector<CpuFrame> cpu;
    std::vector<FrameTiming> timings;
    std::vector<double> clocks;
    uint64_t firstMeasured = UINT64_MAX;
    const auto start = std::chrono::steady_clock::now();
    HarnessResult result;
    result.label = options.label;
    result.resolution = resolution;

    for (uint64_t frame = 0;; ++frame)
    {
        const uint32_t slot = (uint32_t)(frame % options.framesInFlight);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_device.queue((QueueType)q).waitCpu(slotFence[slot][q]);
        profiler.beginFrame(frame);
        if (frame >= options.framesInFlight)
        {
            const FrameTiming* t = profiler.lastCompleted();
            const uint64_t done = frame - options.framesInFlight;
            if (t && t->frame == done && done >= firstMeasured && timings.size() < options.frames) timings.push_back(*t);
        }
        if (timings.size() >= options.frames) break;

        const auto t0 = std::chrono::steady_clock::now();
        build(graph, resolution, frame);
        graph.execute(&profiler);
        const double total = msSince(t0);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) slotFence[slot][q] = graph.lastFence((QueueType)q);

        const bool measuring = firstMeasured != UINT64_MAX;
        if (measuring)
        {
            if (cpu.size() < options.frames) cpu.push_back({ total, graph.stats().cpuRecordMs, graph.stats().cpuSubmitMs });
            if (frame % 16 == 0) clocks.push_back((double)sampleSmClockMHz());
        }
        else if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.warmupSeconds)
            firstMeasured = frame + 1;
    }
    m_device.waitIdle();
    result.graph = graph.stats();

    std::vector<double> gpu, cpuTotal, cpuRecord, cpuSubmit;
    std::map<std::string, std::vector<double>> perPass;
    for (const FrameTiming& t : timings)
    {
        gpu.push_back(t.gpuFrameMs);
        std::map<std::string, double> frameSum;
        for (const PassTiming& p : t.passes) frameSum[p.name] += p.durationMs();
        for (auto& [name, ms] : frameSum) perPass[name].push_back(ms);
    }
    if (!timings.empty())
        for (const PassTiming& p : timings.front().passes)
            if (std::find(result.passOrder.begin(), result.passOrder.end(), p.name) == result.passOrder.end()) result.passOrder.push_back(p.name);
    for (const CpuFrame& c : cpu)
    {
        cpuTotal.push_back(c.total);
        cpuRecord.push_back(c.record);
        cpuSubmit.push_back(c.submit);
    }
    result.gpuFrameMs = Distribution::of(gpu);
    result.cpuFrameMs = Distribution::of(cpuTotal);
    result.cpuRecordMs = Distribution::of(cpuRecord);
    result.cpuSubmitMs = Distribution::of(cpuSubmit);
    result.smClockMHz = Distribution::of(clocks);
    for (auto& [name, v] : perPass) result.passMs[name] = Distribution::of(v);

    if (!options.outputDirectory.empty())
    {
        const std::string base = format("%s_%s_%s", options.label.c_str(), resolution.name.c_str(), stamp().c_str());
        std::filesystem::create_directories(options.outputDirectory);
        if (options.writePassCsv && options.passTimestamps)
        {
            std::ostringstream csv;
            csv << "frame,pass,queue,begin_ms,end_ms,duration_ms\n";
            for (const FrameTiming& t : timings)
                for (const PassTiming& p : t.passes)
                    csv << t.frame << "," << p.name << "," << queueName(p.queue) << "," << format("%.6f,%.6f,%.6f", p.beginMs, p.endMs, p.durationMs()) << "\n";
            result.csvPath = options.outputDirectory / (base + ".csv");
            writeTextFile(result.csvPath, csv.str());
        }
        const DeviceCaps& caps = m_device.caps();
        std::ostringstream js;
        js << "{\n";
        js << " \"label\": " << jsonString(options.label) << ",\n";
        js << " \"resolution\": " << jsonString(format("%ux%u", resolution.width, resolution.height)) << ",\n";
        js << " \"quality_sha256\": " << jsonString(m_quality.hash()) << ",\n";
        js << " \"quality_file\": " << jsonString(m_quality.origin()) << ",\n";
        js << " \"build\": {\"commit\": " << jsonString(UNX_BUILD_COMMIT) << ", \"dirty\": " << (UNX_BUILD_DIRTY ? "true" : "false") << ", \"diff_sha256\": " << jsonString(UNX_BUILD_DIFF_SHA256) << "},\n";
        js << " \"adapter\": " << jsonString(caps.adapter) << ", \"driver\": " << jsonString(caps.driver) << ",\n";
        js << " \"d3d12core\": " << jsonString(caps.runtimeVersion) << ", \"agility_package\": " << jsonString(UNX_AGILITY_PACKAGE_VERSION) << ",\n";
        js << " \"nvapi\": " << jsonString(caps.nvapiInterface + " / driver branch " + caps.nvapiBranch) << ",\n";
        js << " \"gpu_lock\": " << jsonString(lockHolder) << ",\n";
        js << " \"queue_priority\": " << jsonString(m_device.options().queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high" : "normal") << ",\n";
        js << " \"warmup_s\": " << options.warmupSeconds << ", \"frames\": " << timings.size() << ", \"frames_in_flight\": " << options.framesInFlight << ", \"pass_timestamps\": " << (options.passTimestamps ? "true" : "false")
           << ", \"async_compute\": " << (options.asyncCompute ? "true" : "false") << ",\n";
        js << " \"gpu_frame_ms\": " << jsonDistribution(result.gpuFrameMs) << ",\n";
        js << " \"cpu_frame_ms\": " << jsonDistribution(result.cpuFrameMs) << ",\n";
        js << " \"cpu_record_ms\": " << jsonDistribution(result.cpuRecordMs) << ",\n";
        js << " \"cpu_submit_ms\": " << jsonDistribution(result.cpuSubmitMs) << ",\n";
        js << " \"sm_clock_mhz\": " << jsonDistribution(result.smClockMHz) << ",\n";
        const RenderGraphStats& g = result.graph;
        js << " \"graph\": {\"declared_passes\": " << g.declaredPasses << ", \"live_passes\": " << g.livePasses << ", \"transients\": " << g.transientResources
           << ", \"barrier_batches\": " << g.barrierBatches << ", \"barriers\": " << g.barriers << ", \"cross_queue_syncs\": " << g.crossQueueSyncs
           << ", \"command_lists\": " << g.commandLists << ", \"transient_bytes_aliased\": " << g.transientBytesAliased << ", \"transient_bytes_unaliased\": " << g.transientBytesUnaliased << "},\n";
        js << " \"passes\": {\n";
        for (size_t i = 0; i < result.passOrder.size(); ++i)
        {
            const std::string& name = result.passOrder[i];
            js << "  " << jsonString(name) << ": " << jsonDistribution(result.passMs[name]) << (i + 1 < result.passOrder.size() ? ",\n" : "\n");
        }
        js << " }\n}\n";
        result.jsonPath = options.outputDirectory / (base + ".json");
        writeTextFile(result.jsonPath, js.str());
    }
    return result;
}

void Harness::printSummary(const HarnessResult& r) const
{
    logf("[%s %s] gpu frame median %.4f ms, p95 %.4f, p99 %.4f (n=%u) | cpu frame median %.4f ms (record %.4f, submit %.4f) | SM clock median %.0f MHz (min %.0f) | "
         "passes %u, barriers %u in %u batches, cross-queue syncs %u, command lists %u, transients %u (%.1f MB aliased / %.1f MB unaliased) | quality %s\n",
         r.label.c_str(), r.resolution.name.c_str(), r.gpuFrameMs.median, r.gpuFrameMs.p95, r.gpuFrameMs.p99, r.gpuFrameMs.count, r.cpuFrameMs.median,
         r.cpuRecordMs.median, r.cpuSubmitMs.median, r.smClockMHz.median, r.smClockMHz.min, r.graph.livePasses, r.graph.barriers, r.graph.barrierBatches,
         r.graph.crossQueueSyncs, r.graph.commandLists, r.graph.transientResources, r.graph.transientBytesAliased / 1048576.0, r.graph.transientBytesUnaliased / 1048576.0,
         m_quality.shortHash().c_str());
}
} // namespace unx::render
