// Mesh card generation off the frame and its disk cache (unx/refl/MeshCardCache.h).
#include "unx/refl/MeshCardCache.h"

#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace unx::render::refl
{
namespace
{
// Changes when scene::buildMeshCards' rules or this file's format change: older files are then ignored.
constexpr uint32_t kGeneratorVersion = 3;
constexpr char kMagic[8] = { 'U', 'N', 'X', 'M', 'C', 'R', 'D', '1' };

struct Key
{
    uint32_t mesh;
    uint32_t scaleBits;
    uint64_t identity;
    bool operator<(const Key& o) const
    {
        if (mesh != o.mesh) return mesh < o.mesh;
        if (scaleBits != o.scaleBits) return scaleBits < o.scaleBits;
        return identity < o.identity;
    }
};

struct Entry
{
    std::atomic<bool> ready{ false };
    scene::MeshCards cards;
};

struct Job
{
    Entry* entry = nullptr;
    scene::Mesh mesh;                     // positions, indices and submeshes only (a copy: the scene may be edited meanwhile)
    std::vector<uint8_t> triangleTwoSided;
    float scale = 1;
};

std::string hexOf(const std::array<uint8_t, 32>& h)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i)  // 128 bits name the file
    {
        s.push_back(digits[h[i] >> 4]);
        s.push_back(digits[h[i] & 15]);
    }
    return s;
}

std::string contentName(const Job& job)
{
    Sha256 sha;
    sha.update(&kGeneratorVersion, sizeof kGeneratorVersion);
    sha.update(&job.scale, sizeof job.scale);
    const uint64_t counts[3] = { job.mesh.positions.size(), job.mesh.indices.size(), job.triangleTwoSided.size() };
    sha.update(counts, sizeof counts);
    if (!job.mesh.positions.empty()) sha.update(job.mesh.positions.data(), job.mesh.positions.size() * sizeof(float3));
    if (!job.mesh.indices.empty()) sha.update(job.mesh.indices.data(), job.mesh.indices.size() * sizeof(uint32_t));
    if (!job.triangleTwoSided.empty()) sha.update(job.triangleTwoSided.data(), job.triangleTwoSided.size());
    return hexOf(sha.finish());
}

bool readCards(const std::filesystem::path& path, scene::MeshCards& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[8];
    uint32_t version = 0, count = 0, twoSided = 0, surfels = 0;
    in.read(magic, 8);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&count), 4);
    in.read(reinterpret_cast<char*>(&twoSided), 4);
    in.read(reinterpret_cast<char*>(&surfels), 4);
    if (!in || std::memcmp(magic, kMagic, 8) != 0 || version != kGeneratorVersion || count > 4096) return false;
    float bounds[6];
    in.read(reinterpret_cast<char*>(bounds), sizeof bounds);
    out.boundsMin = { bounds[0], bounds[1], bounds[2] };
    out.boundsMax = { bounds[3], bounds[4], bounds[5] };
    out.mostlyTwoSided = twoSided != 0;
    out.surfels = surfels;
    out.cards.resize(count);
    for (scene::MeshCard& c : out.cards)
    {
        float v[6];
        uint32_t direction = 0;
        in.read(reinterpret_cast<char*>(v), sizeof v);
        in.read(reinterpret_cast<char*>(&direction), 4);
        if (direction >= scene::kMeshCardDirections) return false;
        c.origin = { v[0], v[1], v[2] };
        c.extent = { v[3], v[4], v[5] };
        c.direction = direction;
    }
    return (bool)in;
}

void writeCards(const std::filesystem::path& path, const scene::MeshCards& cards)
{
    // (written under another name and renamed: a reader never sees half a file; two processes writing the same cards
    // write the same bytes)
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        const uint32_t count = (uint32_t)cards.cards.size(), twoSided = cards.mostlyTwoSided ? 1u : 0u, surfels = cards.surfels;
        out.write(kMagic, 8);
        out.write(reinterpret_cast<const char*>(&kGeneratorVersion), 4);
        out.write(reinterpret_cast<const char*>(&count), 4);
        out.write(reinterpret_cast<const char*>(&twoSided), 4);
        out.write(reinterpret_cast<const char*>(&surfels), 4);
        const float bounds[6] = { cards.boundsMin.x, cards.boundsMin.y, cards.boundsMin.z, cards.boundsMax.x, cards.boundsMax.y, cards.boundsMax.z };
        out.write(reinterpret_cast<const char*>(bounds), sizeof bounds);
        for (const scene::MeshCard& c : cards.cards)
        {
            const float v[6] = { c.origin.x, c.origin.y, c.origin.z, c.extent.x, c.extent.y, c.extent.z };
            out.write(reinterpret_cast<const char*>(v), sizeof v);
            out.write(reinterpret_cast<const char*>(&c.direction), 4);
        }
        if (!out) return;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) std::filesystem::remove(temp, ec);
}
} // namespace

std::filesystem::path defaultMeshCardDirectory()
{
    char buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    std::error_code ec;
    const std::filesystem::path base = n > 0 && n < MAX_PATH ? std::filesystem::path(buffer) : std::filesystem::temp_directory_path(ec);
    return base / "UnravelNext" / "MeshCards";
}

struct MeshCardCache::Impl
{
    std::filesystem::path directory;
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::map<Key, std::unique_ptr<Entry>> entries;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    uint32_t running = 0;
    bool stop = false;
    std::atomic<uint32_t> pending{ 0 };
    uint64_t generated = 0, loaded = 0;
    double generateMs = 0;

    void work()
    {
        // (below the frame's threads: the generation yields to the renderer and to the game)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
                ++running;
            }
            scene::MeshCards cards;
            bool fromDisk = false;
            std::filesystem::path file;
            try
            {
                if (!directory.empty())
                {
                    file = directory / (contentName(job) + ".mcards");
                    fromDisk = readCards(file, cards);
                }
                if (!fromDisk)
                {
                    cards = scene::buildMeshCards(job.mesh, job.triangleTwoSided, scene::kMaxMeshCards, job.scale);
                    if (!file.empty()) writeCards(file, cards);
                }
            }
            catch (const std::exception& e)
            {
                logf("mesh cards: generation failed (%s): the mesh gets no cards\n", e.what());
                cards = scene::MeshCards{};
            }
            job.entry->cards = std::move(cards);
            job.entry->ready.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mutex);
                --running;
                if (fromDisk) ++loaded;
                else
                {
                    ++generated;
                    generateMs += job.entry->cards.stats.bvhMs + job.entry->cards.stats.columnMs + job.entry->cards.stats.visibilityMs + job.entry->cards.stats.clusterMs;
                }
                pending.fetch_sub(1, std::memory_order_release);  // (under the lock: clear() resets the count there)
            }
            idle.notify_all();
        }
    }
};

MeshCardCache::MeshCardCache(std::filesystem::path directory) : m(std::make_unique<Impl>())
{
    m->directory = std::move(directory);
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 8u);
    for (uint32_t i = 0; i < threads; ++i) m->workers.emplace_back([impl = m.get()] { impl->work(); });
}

MeshCardCache::~MeshCardCache()
{
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        m->stop = true;
        m->queue.clear();
    }
    m->wake.notify_all();
    for (std::thread& t : m->workers) t.join();
}

const scene::MeshCards* MeshCardCache::find(const scene::Scene& scene, uint32_t mesh, float scale, uint64_t identity)
{
    if (mesh >= scene.meshes.size() || !(scale > 0)) return nullptr;
    // The generator's lengths are world metres (10 cm voxels), so its result depends on the scale only through which side
    // of those thresholds the mesh's details fall: instances whose scales differ by less than a quarter octave share one
    // generation (a forest of one tree at 200 random scales is one job, not 200). The cards are in mesh units either way.
    scale = std::exp2(std::round(2.0f * std::log2(scale)) * 0.5f);
    Key key;
    key.mesh = mesh;
    std::memcpy(&key.scaleBits, &scale, 4);
    key.identity = identity;
    Entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        auto it = m->entries.find(key);
        if (it != m->entries.end()) entry = it->second.get();
    }
    if (entry) return entry->ready.load(std::memory_order_acquire) ? &entry->cards : nullptr;

    // A new request: what the generator reads, copied (the scene's vectors may move under an edit while a worker runs).
    const scene::Mesh& source = scene.meshes[mesh];
    Job job;
    job.scale = scale;
    job.mesh.positions = source.positions;
    job.mesh.indices = source.indices;
    job.mesh.submeshes = source.submeshes;
    job.triangleTwoSided.assign(source.indices.size() / 3, 0);
    bool anyTwoSided = false;
    for (const scene::Submesh& sub : source.submeshes)
    {
        if (sub.material >= scene.materials.size() || !scene.materials[sub.material].twoSided) continue;
        anyTwoSided = true;
        const size_t first = sub.indexOffset / 3, last = std::min(job.triangleTwoSided.size(), (size_t)(sub.indexOffset + sub.indexCount) / 3);
        for (size_t t = first; t < last; ++t) job.triangleTwoSided[t] = 1;
    }
    if (!anyTwoSided) job.triangleTwoSided.clear();
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        std::unique_ptr<Entry>& slot = m->entries[key];
        if (!slot)
        {
            slot = std::make_unique<Entry>();
            job.entry = slot.get();
            m->pending.fetch_add(1, std::memory_order_release);
            m->queue.push_back(std::move(job));
        }
    }
    m->wake.notify_one();
    return nullptr;
}

uint32_t MeshCardCache::pending() const { return m->pending.load(std::memory_order_acquire); }

void MeshCardCache::wait()
{
    std::unique_lock<std::mutex> lock(m->mutex);
    m->idle.wait(lock, [&] { return m->queue.empty() && m->running == 0; });
}

void MeshCardCache::clear()
{
    {
        std::unique_lock<std::mutex> lock(m->mutex);
        m->queue.clear();
        m->idle.wait(lock, [&] { return m->running == 0; });
        if (m->generated + m->loaded > 0)
            logf("mesh cards: %llu meshes generated (%.1f s of worker time), %llu read from %s\n", (unsigned long long)m->generated, m->generateMs / 1000.0,
                 (unsigned long long)m->loaded, m->directory.empty() ? "(no directory)" : m->directory.string().c_str());
        m->entries.clear();
        m->generated = m->loaded = 0;
        m->generateMs = 0;
    }
    m->pending.store(0, std::memory_order_release);
}
} // namespace unx::render::refl
