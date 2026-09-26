#pragma once
// Time-sliced use of the GPU measurement lock (INTERFACES_KO.md 3.3) by a long GPU job. A reference render takes
// minutes, so holding the lock for all of it (GpuLock.ps1 around the process) would stop every timing run meanwhile.
// Instead the job holds the lock in slices of <= kSliceSeconds of GPU work with kind "correctness" (background CPU jobs
// keep running; timing measurements wait at most one slice) and pauses while .gpulock/HOLD exists (the user plays).
// Same protocol as Tools/CI/GpuLock.ps1: the named mutex Local\UnravelNext.GpuMeasurement, .gpulock/current.json
// replaced atomically, "acquire" / "release" / "stale release" lines in .gpulock/history.log ("slice k/N"), the waiter
// record .gpulock/waiting/<pid>.json while waiting; correctness acquisitions yield to a live timing waiter (INTERFACES
// 3.3 v1.40). Interim C implementation
// until core's unx::GpuLockSlice (GpuLock.h) lands.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace unx::reference::gpu
{
class GpuSlice
{
public:
    static constexpr double kSliceSeconds = 15.0;

    // lockDir: <repo>/.gpulock. what: the command text for current.json and history.log.
    GpuSlice(std::filesystem::path lockDir, std::string track, std::string what);
    ~GpuSlice();
    GpuSlice(const GpuSlice&) = delete;
    GpuSlice& operator=(const GpuSlice&) = delete;

    // Waits for HOLD to disappear and for the mutex; returns the seconds waited.
    double acquire();
    void release();
    bool held() const { return m_held; }
    // True once the current slice has lasted kSliceSeconds (the caller finishes its in-flight GPU work, then releases).
    bool sliceExpired() const;
    uint32_t slices() const { return m_slices; }
    // Expected number of slices for the history lines ("slice k/N"); 0 = unknown ("k/?").
    void setTotalEstimate(uint32_t n) { m_totalEstimate = n; }
    double waitedSeconds() const { return m_waited; }
    double longestSliceSeconds() const { return m_longest; }

private:
    std::filesystem::path m_dir;
    std::string m_track, m_what;
    void* m_mutex = nullptr;
    bool m_held = false;
    uint32_t m_slices = 0, m_totalEstimate = 0;
    double m_waited = 0, m_longest = 0;
    std::chrono::steady_clock::time_point m_start;
};
} // namespace unx::reference::gpu
