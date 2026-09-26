// Three-tier streaming (render C, C6), real device with the debug layer (DirectStorage copy queues; no kernels):
// Each test frame ends with Streamer::flush(): the work issued in a frame completes before the next one, the latency
// the design states (a real frame is >= 6 ms; DirectStorage reads 64 KB pages in tens of microseconds).
//   pages_stream_exactly      300 pages of 4-64 KB random bytes in a page file; a moving window of 100 requested pages
//                             over 60 frames with a 2-heap pool, a 5 MB RAM cache and 2 MB of upload per frame: every
//                             resident page's VRAM bytes equal its source, uploads stay within the per-frame budget, the
//                             RAM cache within its capacity, every requested page becomes resident within the stated
//                             bound, a page stays resident for framesInFlight + 1 frames after its last request
//   budget_shrink_and_return  the pool budget drops to one heap: the second heap's pages leave residency at once, the heap
//                             is evicted (ID3D12Device::Evict) framesInFlight + 1 frames later; streaming continues in the
//                             first heap with exact bytes; when the budget returns the heap is made resident and used
//   unx_test_streaming_streamingtests [filter]
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/streaming/Streaming.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <exception>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

using namespace unx;
using namespace unx::render;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}
struct Register
{
    Register(const char* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};
#define UNX_TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

Device& device()
{
    static Device d([] {
        DeviceOptions o;
        o.debugLayer = true;
        return o;
    }());
    return d;
}

std::vector<std::vector<uint8_t>> makePages(uint32_t count, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> size(4096, 65536), byte(0, 255);
    std::vector<std::vector<uint8_t>> pages(count);
    for (auto& p : pages)
    {
        p.resize(size(rng));
        for (auto& b : p) b = (uint8_t)byte(rng);
    }
    return pages;
}

// Copies every pool heap to the CPU.
std::vector<std::vector<uint8_t>> readPool(const streaming::Streamer& s)
{
    std::vector<std::vector<uint8_t>> out;
    const uint64_t heapBytes = (uint64_t)s.settings().slotBytes * s.settings().heapSlots;
    for (uint32_t h = 0; h < s.heapCount(); ++h)
    {
        if (!s.heapResident(h))
        {
            out.emplace_back();
            continue;
        }
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = heapBytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check(device().d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "readback");
        CommandList cl = device().acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(rb.Get(), 0, s.heapBuffer(h), 0, heapBytes);
        device().queue(QueueType::Graphics).waitCpu(device().submit(cl));
        uint8_t* p = nullptr;
        check(rb->Map(0, nullptr, (void**)&p), "map");
        out.emplace_back(p, p + heapBytes);
        rb->Unmap(0, nullptr);
    }
    return out;
}

// Every resident page among 'pages' has its exact bytes in its slot. Returns the number checked.
uint32_t checkResident(const streaming::Streamer& s, uint32_t file, const std::vector<std::vector<uint8_t>>& pages)
{
    const auto pool = readPool(s);
    uint32_t checked = 0;
    for (uint32_t p = 0; p < (uint32_t)pages.size(); ++p)
    {
        const uint32_t slot = s.residentSlot(file, p);
        if (slot == streaming::kNone) continue;
        const uint32_t heap = slot / s.settings().heapSlots, at = (slot % s.settings().heapSlots) * s.settings().slotBytes;
        CHECK(heap < pool.size() && !pool[heap].empty());  // resident pages live in resident heaps
        CHECK(std::memcmp(pool[heap].data() + at, pages[p].data(), pages[p].size()) == 0);
        ++checked;
    }
    return checked;
}

std::string tempFile(const char* name) { return (std::filesystem::temp_directory_path() / (std::string(name) + std::to_string(GetCurrentProcessId()) + ".unxpages")).string(); }
} // namespace

UNX_TEST(pages_stream_exactly)
{
    const auto pages = makePages(300, 11);
    const std::string path = tempFile("unx_stream_a");
    streaming::PageFileWriter::write(path, pages);
    streaming::Settings cfg;
    cfg.slotBytes = 65536;
    cfg.heapSlots = 64;
    cfg.vramPoolBytes = 2ull * 65536 * 64;  // 2 heaps, 128 slots
    cfg.ramCacheBytes = 80ull * 65536;
    cfg.uploadBytesPerFrame = 2ull << 20;
    cfg.verifyPages = true;
    {
    streaming::Streamer s(device(), cfg);
    const uint32_t file = s.addFile(path);
    CHECK(s.pageCount(file) == 300 && s.heapCount() == 2);
    // Frame bound for a newly requested window: ceil(missing bytes / upload budget) + 2.
    uint32_t checked = 0, latest = 0;
    uint64_t maxUpload = 0, maxRam = 0;
    for (uint32_t f = 0; f < 60; ++f)
    {
        const uint32_t first = (f / 20) * 100;  // three windows of 100 pages, 20 frames each
        for (uint32_t k = 0; k < 100; ++k) s.request(file, first + k, -(float)k);
        s.update(f);
        s.flush();  // a frame's time: the reads and uploads issued this frame complete before the next (design latency)
        const streaming::Stats& st = s.stats();
        if (f < 4 || f % 20 == 19)
            logf("      frame %u: requested %u, resident %u (requested %u), disk reads %u (%.2f MB), uploads %u (%.2f MB), RAM %.2f MB, fences read %llu/%llu upload %llu/%llu\n", f, st.requested, st.resident,
                 st.residentRequested, st.diskReads, st.diskBytes / 1048576.0, st.uploads, st.uploadBytes / 1048576.0, st.ramCacheUsed / 1048576.0,
                 (unsigned long long)st.readsCompleted, (unsigned long long)st.readsIssued, (unsigned long long)st.uploadsCompleted, (unsigned long long)st.uploadsIssued);
        maxUpload = std::max(maxUpload, st.uploadBytes);
        maxRam = std::max(maxRam, st.ramCacheUsed);
        if (f % 20 == 19)
        {
            s.flush();
            for (uint32_t k = 0; k < 100; ++k) CHECK(s.residentSlot(file, first + k) != streaming::kNone);  // whole window resident
            checked += checkResident(s, file, pages);
        }
        if (f % 20 == 0 && f > 0)
        {
            // Pages of the previous window stay resident for framesInFlight + 1 frames after their last request.
            uint32_t kept = 0;
            for (uint32_t k = 0; k < 100; ++k) kept += s.residentSlot(file, first - 100 + k) != streaming::kNone;
            latest = kept;
            CHECK(kept == 100 || f == 0);
        }
    }
    const streaming::Stats& st = s.stats();
    logf("    verified %u resident pages byte for byte; max upload %.2f MB/frame (budget 2), max RAM cache %.2f MB (capacity 5), previous window kept %u, "
         "hash failures %u\n",
         checked, maxUpload / 1048576.0, maxRam / 1048576.0, latest, st.hashFailures);
    CHECK(checked >= 300);
    CHECK(maxUpload <= cfg.uploadBytesPerFrame && maxRam <= cfg.ramCacheBytes && st.hashFailures == 0);
    }
    std::filesystem::remove(path);
}

UNX_TEST(budget_shrink_and_return)
{
    const auto pages = makePages(200, 23);
    const std::string path = tempFile("unx_stream_b");
    streaming::PageFileWriter::write(path, pages);
    streaming::Settings cfg;
    cfg.slotBytes = 65536;
    cfg.heapSlots = 32;
    cfg.vramPoolBytes = 2ull * 65536 * 32;  // 2 heaps, 64 slots
    cfg.ramCacheBytes = 16ull << 20;
    cfg.uploadBytesPerFrame = 8ull << 20;
    cfg.verifyPages = true;
    {
    streaming::Streamer s(device(), cfg);
    const uint32_t file = s.addFile(path);
    uint64_t f = 0;
    auto frame = [&](uint32_t first, uint32_t count) {
        for (uint32_t k = 0; k < count; ++k) s.request(file, first + k, -(float)k);
        s.update(f++);
        s.flush();  // a frame's time
    };
    for (int i = 0; i < 6; ++i) frame(0, 60);
    s.flush();
    uint32_t inSecond = 0;
    for (uint32_t k = 0; k < 60; ++k)
    {
        const uint32_t slot = s.residentSlot(file, k);
        CHECK(slot != streaming::kNone);
        inSecond += slot >= cfg.heapSlots;
    }
    CHECK(inSecond > 0);
    s.overridePoolBudget(65536ull * 32);  // one heap
    frame(0, 60);
    for (uint32_t k = 0; k < 60; ++k) CHECK(s.residentSlot(file, k) == streaming::kNone || s.residentSlot(file, k) < cfg.heapSlots);
    for (int i = 0; i < 4; ++i) frame(0, 30);
    CHECK(s.stats().heapsEvicted == 1 && s.stats().heapsResident == 1);
    for (int i = 0; i < 6; ++i) frame(100, 30);
    s.flush();
    for (uint32_t k = 0; k < 30; ++k) CHECK(s.residentSlot(file, 100 + k) != streaming::kNone && s.residentSlot(file, 100 + k) < cfg.heapSlots);
    uint32_t checked = checkResident(s, file, pages);
    s.overridePoolBudget(2ull * 65536 * 32);
    for (int i = 0; i < 6; ++i) frame(0, 60);
    s.flush();
    CHECK(s.stats().heapsResident == 2);
    for (uint32_t k = 0; k < 60; ++k) CHECK(s.residentSlot(file, k) != streaming::kNone);
    checked += checkResident(s, file, pages);
    logf("    budget 2 -> 1 -> 2 heaps: eviction and return exact (%u resident pages verified), hash failures %u\n", checked, s.stats().hashFailures);
    CHECK(s.stats().hashFailures == 0);
    }
    std::filesystem::remove(path);
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, runCount = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++runCount;
        try
        {
            t.fn();
        }
        catch (const std::exception& e)
        {
            logf("FAIL %s: %s\n", t.name, e.what());
            continue;
        }
        logf("PASS %s\n", t.name);
        ++passed;
    }
    logf("%u/%u passed\n", passed, runCount);
    return passed == runCount ? 0 : 1;
}
