#pragma once
// GPU contention of a measurement window for the host gates (I track): GpuLock.ps1's sampler writes the samples in which
// another process kept the GPU busy (>= 50 ms/s) to the file named by UNX_GPU_CONTENTION (one "{"t_ms": ...}" per line,
// the interval in "interval_ms"; INTERFACES v1.39). A frame submitted inside a contended interval is contended; the rest
// give the uncontended frame time. Same rules as render::Harness.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace unx::host::gate
{
inline int64_t unixMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Contention
{
    bool sampled = false;              // the run had a sampler (under GpuLock timing)
    int intervalMs = 1000;
    std::vector<long long> stamps;     // contended intervals (end stamps) inside the window
    std::vector<std::string> samples;  // the sample lines, as written
    double seconds() const { return (double)stamps.size() * intervalMs / 1000.0; }
    // A frame submitted at unix ms 'submitted' lies in a contended interval.
    bool contended(int64_t submitted) const
    {
        for (long long t : stamps)
            if (submitted <= t && submitted > t - intervalMs) return true;
        return false;
    }
};

inline Contention readContention(int64_t from, int64_t to)
{
    Contention c;
    char path[1024];
    size_t n = 0;
    if (getenv_s(&n, path, sizeof path, "UNX_GPU_CONTENTION") != 0 || n <= 1) return c;
    std::ifstream in(path);
    if (!in) return c;
    c.sampled = true;
    std::string line;
    while (std::getline(in, line))
    {
        const size_t i = line.find("\"interval_ms\": ");
        if (i != std::string::npos) c.intervalMs = std::atoi(line.c_str() + i + 15);
        long long t = 0;
        const size_t k = line.find("{\"t_ms\": ");
        if (k == std::string::npos || sscanf_s(line.c_str() + k, "{\"t_ms\": %lld", &t) != 1) continue;
        if (t < from || t > to + c.intervalMs) continue;
        std::string sample = line.substr(k);
        while (!sample.empty() && (sample.back() == ',' || sample.back() == ' ' || sample.back() == '\r')) sample.pop_back();
        c.samples.push_back(sample);
        c.stamps.push_back(t);
    }
    std::sort(c.stamps.begin(), c.stamps.end());
    c.stamps.erase(std::unique(c.stamps.begin(), c.stamps.end()), c.stamps.end());
    return c;
}
} // namespace unx::host::gate
