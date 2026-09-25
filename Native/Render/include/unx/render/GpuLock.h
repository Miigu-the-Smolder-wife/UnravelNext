#pragma once
// GPU measurement lock (INTERFACES_KO.md 3.3). Performance measurements run one at a time across all sessions:
// Tools/CI/GpuLock.ps1 holds a named mutex while it runs the measuring command and sets UNX_GPU_LOCK=<track> for it.
// Correctness runs (tests, validation, reference comparisons) do not take the lock, except a new kernel's first hardware
// run and long GPU correctness work, which take it as kind "correctness".
#include <chrono>
#include <cstdint>
#include <string>

namespace unx::render
{
// Throws unless the process runs under GpuLock.ps1 (or holds a GpuLockSlice). Every performance tool (Harness::run,
// microbench-style experiments, gates) calls it before measuring. Returns the holder's track name.
std::string requireGpuLock(const char* what);

// The lock taken from inside a process, one slice at a time (v1.42, C request: the GPU reference path tracer renders for
// minutes, so it holds the lock per slice of <= 15 s and timing measurements get in between). The protocol is
// GpuLock.ps1's: the named mutex "Local\UnravelNext.GpuMeasurement", .gpulock/current.json replaced atomically, history
// lines ("acquire <track> (<kind>) :: <what> <label>", "release <track> (<kind>) exit <code> <label> <ms> ms", LONG_SLICE
// past 15 s), a waiting file .gpulock/waiting/<pid>.json while waiting, correctness yielding to live timing waiters
// (checked again right after the mutex is taken), no acquire while .gpulock/HOLD exists, stale holders logged. While a
// slice is held, UNX_GPU_LOCK = track in this process. acquire and release must run on the same thread (a Win32 mutex
// belongs to the thread that took it).
class GpuLockSlice
{
public:
    // kind: "timing" or "correctness". lockDir: the .gpulock folder; empty = UNX_GPU_LOCK_DIR, else the first ".gpulock"
    // or git root found walking up from the current directory (run with the repository as the working directory).
    // mutexName: tests only.
    GpuLockSlice(std::string track, std::string kind, std::string what, std::string lockDir = {},
                 std::string mutexName = "Local\\UnravelNext.GpuMeasurement");
    ~GpuLockSlice();
    GpuLockSlice(const GpuLockSlice&) = delete;
    GpuLockSlice& operator=(const GpuLockSlice&) = delete;

    // Waits up to waitLimit (HOLD, a timing waiter for kind correctness, or another holder); false = not acquired.
    // 'label' names the slice in the history lines (e.g. "slice 3/40").
    bool acquire(std::chrono::milliseconds waitLimit, const std::string& label = {});
    void release(int exitCode = 0);
    bool held() const { return m_held; }
    // What the last acquire waited for (empty: nothing), e.g. "HOLD: playing" or "timing waiter S (pid 1234)".
    const std::string& lastBlocker() const { return m_lastBlocker; }
    const std::string& lockDir() const { return m_dir; }

private:
    std::string blocker();
    void appendHistory(const std::string& line);

    std::string m_track, m_kind, m_what, m_dir, m_label, m_lastBlocker;
    void* m_mutex = nullptr;
    bool m_held = false;
    std::chrono::steady_clock::time_point m_since;
};
} // namespace unx::render
