#pragma once
#include "unx/core/Config.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/RenderGraph.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace unx::render
{
struct Resolution
{
    uint32_t width = 0, height = 0;
    std::string name;  // "4K" / "1440p" / "1080p"
};

// Measurement happens only at the target resolutions (ARCHITECTURE_KO.md 6: the harness refuses others). The
// accepted set is the intersection of {3840x2160, 2560x1440, 1920x1080} and quality key output.resolutions.
Resolution resolutionFromString(const std::string& text, const QualityConfig& quality);

struct HarnessOptions
{
    double warmupSeconds = 1.5;  // ARCHITECTURE 1.1: clocks ramp for ~1 s; shorter warm-ups under-measure 2-4x
    // Warm-up by frame count instead of time (-1: warmupSeconds; 0: no warm-up). A time-based warm-up ends at a different frame each run
    // (and differs between configurations of different speed): every temporal state - GI cache convergence, jitter
    // phase, histories - then differs at the captured last frame. Image comparisons need the same frame index.
    int64_t warmupFrames = -1;
    uint32_t frames = 600;
    uint32_t framesInFlight = 2;
    bool passTimestamps = true;
    bool asyncCompute = false;  // RenderGraph::setAsyncCompute
    std::string label;
    std::filesystem::path outputDirectory;  // empty = no files
    bool writePassCsv = true;
    // Workload counters of the run (RENDERER_REDESIGN P0: the quantities the cost formulas use), called after the last
    // frame: JSON members ("key": value, ...) written as the result's "workload" object; empty = none.
    std::function<std::string()> extraJson;
};

struct Distribution
{
    uint32_t count = 0;
    double median = 0, p95 = 0, p99 = 0, mean = 0, min = 0, max = 0;
    static Distribution of(std::vector<double> values);
};

struct HarnessResult
{
    std::string label;
    Resolution resolution;
    Distribution gpuFrameMs;
    uint64_t firstMeasuredFrame = 0;  // frames before the measured ones (the warm-up)
    Distribution cpuFrameMs;    // build + compile + record + submit on the calling thread
    Distribution cpuRecordMs;
    Distribution cpuSubmitMs;
    Distribution smClockMHz;
    std::map<std::string, Distribution> passMs;
    std::vector<std::string> passOrder;
    RenderGraphStats graph;
    // GPU contention in the measurement window (GpuLock.ps1 v1.39 sampler, UNX_GPU_CONTENTION): seconds in which some
    // process outside the measured tree kept the GPU busy >= its threshold, and those samples. -1 = no sampler data.
    double contendedSeconds = -1;
    std::vector<std::string> contentionSamples;  // JSON objects {"t_ms", "pid", "ms_per_s", "name"}
    uint32_t contendedFrames = 0;                // measured frames submitted inside a contended sample
    Distribution gpuFrameMsUncontended;          // gpu frame over the other frames
    // Per queue (graphics, compute): command lists per frame and the time outside passes (GpuProfiler QueueTiming).
    uint32_t queueLists[2] = {};
    Distribution queueHeadMs[2], queueTailMs[2], queueGapMs[2];
    std::filesystem::path jsonPath, csvPath;
};

class Harness
{
public:
    using BuildFrame = std::function<void(RenderGraph& graph, const Resolution& resolution, uint64_t frame)>;

    Harness(Device& device, const QualityConfig& quality);
    ~Harness();
    HarnessResult run(const Resolution& resolution, const HarnessOptions& options, const BuildFrame& build);
    void printSummary(const HarnessResult& result) const;

private:
    uint32_t sampleSmClockMHz();
    Device& m_device;
    const QualityConfig& m_quality;
    void* m_gpu = nullptr;  // NvPhysicalGpuHandle
};
} // namespace unx::render
