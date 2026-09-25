#pragma once
// Material v2 studies (Docs/Design/MATERIAL_LAYERS_KO.md 3). CPU only, at most 4 worker threads, below-normal priority,
// paused while a GPU measurement lock is held.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "HoldRecord.h"

namespace unx::study
{
constexpr unsigned kThreads = 4;

// Workers wait before each item while a timing GPU lock is held (.gpulock/current.json relative to the working
// directory, the UnravelNext root; HoldRecord.h), so studies never load the CPU during another session's performance
// measurement. Correctness runs and the manual HOLD marker do not stop these light 4-thread studies.
inline void waitForMeasurementLock()
{
    while (reference::holdActive(".gpulock/current.json")) std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

inline void parallelFor(uint32_t count, const std::function<void(uint32_t)>& fn)
{
    std::atomic<uint32_t> next{ 0 };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kThreads; ++t)
        workers.emplace_back([&] {
            for (uint32_t i = next++; i < count; i = next++)
            {
                waitForMeasurementLock();
                fn(i);
            }
        });
    for (auto& w : workers) w.join();
}

void thinFilmStudy(const std::string& out);
void metalPresets(const std::string& out);
void clearcoatR1Study(const std::string& out, uint32_t photons, bool msCoat, bool candA = false, bool candB = false, bool candC = false, bool candD = false);
void clearcoatDiag(const std::string& out, uint32_t photons);
void clearcoatSpecPath(const std::string& out, uint32_t photons);
void coatFilmStudy(const std::string& out, uint32_t photons);
void exportTables(const std::string& out);
} // namespace unx::study
