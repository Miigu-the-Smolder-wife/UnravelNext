#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace unx::render
{
// Diagnostic CPU declaration phases, deliberately separate from GPU timestamps.
class CpuFrameTrace
{
    using Clock = std::chrono::steady_clock;
    struct Sink
    {
        std::mutex mutex;
        std::ofstream file;
        Sink()
        {
            wchar_t* path = nullptr; size_t pathChars = 0;
            if (_wdupenv_s(&path, &pathChars, L"UNX_CPU_TRACK_TIMINGS") == 0 && path && *path)
                file.open(std::filesystem::path(path), std::ios::out | std::ios::trunc);
            std::free(path);
            if (file.is_open()) file << "native_frame,phase,cpu_ms\n";
        }
    };
    static Sink& sink() { static Sink value; return value; }
    struct Phase { const char* name; double ms; };
    uint64_t frame;
    bool enabled;
    Clock::time_point previous{};
    std::array<Phase, 40> phases{};
    size_t count = 0;
public:
    bool active() const { return enabled; }
    void value(const char* name, double ms) { if (enabled && count < phases.size()) phases[count++] = {name, ms}; }
    explicit CpuFrameTrace(uint64_t f) : frame(f), enabled(sink().file.is_open()) { if (enabled) previous = Clock::now(); }
    void mark(const char* name)
    {
        if (!enabled || count == phases.size()) return;
        auto now = Clock::now();
        phases[count++] = { name, std::chrono::duration<double, std::milli>(now - previous).count() };
        previous = now;
    }
    ~CpuFrameTrace()
    {
        if (!enabled) return;
        auto& output = sink(); std::lock_guard lock(output.mutex);
        for (size_t i=0; i<count; ++i) output.file << frame << ',' << phases[i].name << ',' << phases[i].ms << '\n';
        output.file.flush();
    }
};
}
