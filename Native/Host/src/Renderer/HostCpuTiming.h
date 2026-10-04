#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace unx::host::detail
{
// Opt-in diagnostic output. No ABI changes, GPU query changes or per-pass allocations.
// Current CPU frame and delayed completed GPU frame have separate identities.
class HostCpuTiming
{
    using Clock = std::chrono::steady_clock;
    struct Sink
    {
        std::mutex mutex;
        std::ofstream file;
        Sink()
        {
            wchar_t* path = nullptr;
            size_t count = 0;
            if (_wdupenv_s(&path, &count, L"UNX_HOST_CPU_TIMINGS") == 0 && path && *path)
                file.open(std::filesystem::path(path), std::ios::out | std::ios::trunc);
            std::free(path);
            if (file)
                file << "cpu_frame,native_frame,width,height,callback_ms,admission_ms,slot_wait_ms,fx_lock_ms,drain_fx_ms,begin_frame_ms,declare_graph_ms,prepare_execute_ms,graph_execute_ms,after_execute_ms,end_frame_ms,plan_lookup_ms,compile_ms,views_ms,record_ms,submit_ms,plan_reused,passes,command_lists,barriers,completed_gpu_frame,gpu_ms,graphics_head_ms,graphics_tail_ms,graphics_gap_ms,compute_head_ms,compute_tail_ms,compute_gap_ms\n";
        }
    };
    static Sink& sink() { static Sink value; return value; }
    bool enabled = false;
    Clock::time_point start{}, previous{};
    std::array<double, 10> phases{};

public:
    bool active() const { return enabled; }
    HostCpuTiming() : enabled(sink().file.is_open())
    {
        if (enabled) start = previous = Clock::now();
    }
    void mark(size_t phase)
    {
        if (!enabled) return;
        const auto now = Clock::now();
        phases[phase] = std::chrono::duration<double, std::milli>(now - previous).count();
        previous = now;
    }
    template<class GraphStats, class FrameStats>
    void finish(uint64_t frame, uint64_t nativeFrame, uint32_t width, uint32_t height, const GraphStats& graph, const FrameStats& completed)
    {
        if (!enabled) return;
        const double total = std::chrono::duration<double, std::milli>(previous - start).count();
        auto& output = sink();
        std::lock_guard lock(output.mutex);
        auto& f = output.file;
        f << std::setprecision(10) << frame << ',' << nativeFrame << ',' << width << ',' << height << ',' << total;
        for (double phase : phases) f << ',' << phase;
        f << ',' << graph.cpuPlanLookupMs << ',' << graph.cpuCompileMs << ',' << graph.cpuViewsMs
          << ',' << graph.cpuRecordMs << ',' << graph.cpuSubmitMs << ',' << graph.planReused
          << ',' << graph.livePasses << ',' << graph.commandLists << ',' << graph.barriers
          << ',' << completed.frameIndex << ',' << completed.gpuMs;
        for (const auto& q : completed.queues) f << ',' << q.headMs << ',' << q.tailMs << ',' << q.gapMs;
        f << '\n';
        f.flush(); // Unity may terminate without invoking DLL static destructors.
    }
};
}
