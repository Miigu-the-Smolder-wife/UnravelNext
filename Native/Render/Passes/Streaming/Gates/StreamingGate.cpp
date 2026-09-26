// Streaming throughput (render C, C6): a 256 MB page file of 64 KB pages streamed NVMe -> RAM -> VRAM with the
// production settings (64 MB of upload per frame), frames paced at 6.06 ms (165 fps), wall time and throughput per tier.
// DirectStorage reads unbuffered, so the OS file cache does not serve them. Timing run: GPU lock.
//   powershell -File Tools/CI/GpuLock.ps1 -Track V -- build/<t>/bin/unx_gate_streaming_streaminggate.exe [--out DIR]
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/GpuLock.h"
#include "unx/streaming/Streaming.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace unx;
using namespace unx::render;

int main(int argc, char** argv)
{
    try
    {
        std::string out = "Results/C/Streaming";
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--out" && i + 1 < argc) out = argv[++i];
        requireGpuLock("unx_gate_streaming_streaminggate");
        Device device(DeviceOptions{});
        const uint32_t pages = 4096, pageBytes = 65536;
        std::vector<std::vector<uint8_t>> data(pages, std::vector<uint8_t>(pageBytes));
        std::mt19937 rng(5);
        for (auto& p : data)
            for (size_t i = 0; i < p.size(); i += 4)
            {
                const uint32_t v = rng();
                std::memcpy(p.data() + i, &v, 4);
            }
        const std::string path = (std::filesystem::temp_directory_path() / ("unx_stream_gate" + std::to_string(GetCurrentProcessId()) + ".unxpages")).string();
        streaming::PageFileWriter::write(path, data);
        data.clear();
        double seconds = 0;
        uint32_t frames = 0;
        streaming::Stats last;
        {
            streaming::Settings cfg;
            cfg.vramPoolBytes = (uint64_t)pages * pageBytes;  // the whole file fits: measures the pipe, not eviction
            cfg.ramCacheBytes = (uint64_t)pages * pageBytes;
            streaming::Streamer s(device, cfg);
            const uint32_t file = s.addFile(path);
            const auto t0 = std::chrono::steady_clock::now();
            auto next = t0;
            for (;; ++frames)
            {
                for (uint32_t p = 0; p < pages; ++p) s.request(file, p, -(float)p);
                s.update(frames);
                last = s.stats();
                if (last.residentRequested == pages) break;
                if (frames > 2000) fail("streaming gate: not resident after 2000 frames");
                next += std::chrono::microseconds(6061);
                std::this_thread::sleep_until(next);
            }
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        std::filesystem::remove(path);
        const double mb = (double)pages * pageBytes / 1048576.0;
        logf("[C6 streaming] %.0f MB in %u frames of 6.06 ms: %.1f ms wall, %.0f MB/s end to end (upload budget 64 MB/frame = %.0f MB/s at 165 fps) | "
             "timing while other GPU/CPU load may run: see the lock line\n",
             mb, frames, seconds * 1000, mb / seconds, 64.0 * 165.0);
        std::filesystem::create_directories(out);
        std::ofstream j(out + "/streaming_gate.json");
        j << "{\"megabytes\": " << mb << ", \"frames\": " << frames << ", \"wall_ms\": " << seconds * 1000 << ", \"mb_per_s\": " << mb / seconds << "}\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("streaming gate failed: %s\n", e.what());
        return 1;
    }
}
