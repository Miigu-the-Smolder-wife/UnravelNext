#pragma once
// Material v2 studies (Docs/Design/MATERIAL_LAYERS_KO.md 3). CPU only, at most 4 worker threads, below-normal priority.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace unx::study
{
constexpr unsigned kThreads = 4;

inline void parallelFor(uint32_t count, const std::function<void(uint32_t)>& fn)
{
    std::atomic<uint32_t> next{ 0 };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kThreads; ++t)
        workers.emplace_back([&] {
            for (uint32_t i = next++; i < count; i = next++) fn(i);
        });
    for (auto& w : workers) w.join();
}

void thinFilmStudy(const std::string& out);
void metalPresets(const std::string& out);
void clearcoatR1Study(const std::string& out, uint32_t photons);
void coatFilmStudy(const std::string& out, uint32_t photons);
void exportTables(const std::string& out);
} // namespace unx::study
