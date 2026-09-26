#include "unx/streaming/Streaming.h"

#include "unx/core/Log.h"

#include <dstorage.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>

namespace unx::streaming
{
using render::ComPtr;
using render::check;

namespace
{
constexpr uint32_t kMagic = 0x50584E55u;  // "UNXP"
constexpr uint32_t kVersion = 1;
constexpr uint64_t kAlign = 4096;

struct PageEntry
{
    uint64_t offset;
    uint32_t bytes;
    uint32_t pad;
    uint64_t fnv;  // FNV-1a 64 of the page bytes (checked when Settings verification is on: tests)
};
static_assert(sizeof(PageEntry) == 24);

uint64_t fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

uint64_t keyOf(uint32_t file, uint32_t page) { return (uint64_t)file << 32 | page; }
} // namespace

void PageFileWriter::write(const std::string& path, const std::vector<std::vector<uint8_t>>& pages)
{
    std::vector<PageEntry> table(pages.size());
    uint64_t offset = (16 + table.size() * sizeof(PageEntry) + kAlign - 1) / kAlign * kAlign;
    for (size_t i = 0; i < pages.size(); ++i)
    {
        table[i] = { offset, (uint32_t)pages[i].size(), 0, fnv1a(pages[i].data(), pages[i].size()) };
        offset += (pages[i].size() + kAlign - 1) / kAlign * kAlign;
    }
    const std::filesystem::path tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) fail("streaming: cannot write %s", tmp.string().c_str());
        const uint32_t head[4] = { kMagic, kVersion, (uint32_t)pages.size(), 0 };
        f.write((const char*)head, 16);
        f.write((const char*)table.data(), (std::streamsize)(table.size() * sizeof(PageEntry)));
        const std::vector<char> zero(kAlign, 0);
        auto pad = [&]() {
            const uint64_t at = (uint64_t)f.tellp(), to = (at + kAlign - 1) / kAlign * kAlign;
            f.write(zero.data(), (std::streamsize)(to - at));
        };
        pad();
        for (const auto& p : pages)
        {
            f.write((const char*)p.data(), (std::streamsize)p.size());
            pad();
        }
        if (!f) fail("streaming: writing %s failed", tmp.string().c_str());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) fail("streaming: rename to %s failed", path.c_str());
}

Budget queryBudget(render::Device& device)
{
    Budget b;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(device.adapter()->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        b.vramBudget = info.Budget;
        b.vramUsage = info.CurrentUsage;
    }
    MEMORYSTATUSEX ms{ sizeof ms };
    if (GlobalMemoryStatusEx(&ms))
    {
        b.ramTotal = ms.ullTotalPhys;
        b.ramAvailable = ms.ullAvailPhys;
    }
    return b;
}

struct Streamer::Impl
{
    render::Device& device;
    Settings cfg;
    Stats stats;
    ComPtr<IDStorageFactory> factory;
    ComPtr<IDStorageQueue> fileQueue, memoryQueue;
    ComPtr<ID3D12Fence> readFence, uploadFence;
    uint64_t readValue = 0, uploadValue = 0;

    struct File
    {
        ComPtr<IDStorageFile> handle;
        std::vector<PageEntry> table;
    };
    std::vector<File> files;

    struct Heap
    {
        ComPtr<ID3D12Heap> heap;
        ComPtr<ID3D12Resource> buffer;
        bool resident = true;
        bool draining = false;       // budget shrank: no new pages; evicted once no frame in flight can read it
        uint64_t drainFrame = 0;
    };
    std::vector<Heap> heaps;
    struct Slot
    {
        uint64_t key = UINT64_MAX;
        uint64_t lastRequested = 0;
        bool pending = false;  // an upload into it is in flight
    };
    std::vector<Slot> slots;

    // RAM cache, least recently requested first.
    struct Ram
    {
        std::vector<uint8_t> bytes;
        uint64_t readValue = 0;  // the read that fills it (ready when readFence >= readValue)
        bool ready = false;
        uint32_t pins = 0;       // uploads in flight from it
        std::list<uint64_t>::iterator order;
    };
    std::unordered_map<uint64_t, Ram> ram;
    std::list<uint64_t> ramOrder;
    uint64_t ramUsed = 0;

    struct Page
    {
        uint32_t slot = kNone;       // resident slot
        uint32_t pendingSlot = kNone;
        uint64_t uploadValue = 0;
        uint64_t lastRequested = 0;
    };
    std::unordered_map<uint64_t, Page> pages;
    std::unordered_map<uint64_t, float> requests;
    uint64_t overrideBudget = 0;
    uint64_t frame = 0;

    explicit Impl(render::Device& d, const Settings& s) : device(d), cfg(s)
    {
        if (!(cfg.slotBytes >= 4096 && cfg.heapSlots > 0)) fail("streaming: slot %u B, %u slots per heap", cfg.slotBytes, cfg.heapSlots);
        check(DStorageGetFactory(IID_PPV_ARGS(&factory)), "DStorageGetFactory (dstorage.dll next to the executable)");
        DSTORAGE_QUEUE_DESC q{};
        q.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        q.Priority = DSTORAGE_PRIORITY_NORMAL;
        q.Device = device.d3d();
        q.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        q.Name = "unx streaming: NVMe -> RAM";
        check(factory->CreateQueue(&q, IID_PPV_ARGS(&fileQueue)), "DirectStorage file queue");
        q.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        q.Name = "unx streaming: RAM -> VRAM";
        check(factory->CreateQueue(&q, IID_PPV_ARGS(&memoryQueue)), "DirectStorage memory queue");
        check(device.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readFence)), "streaming read fence");
        check(device.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence)), "streaming upload fence");
        const Budget b = queryBudget(device);
        if (cfg.ramCacheBytes == 0) cfg.ramCacheBytes = std::min<uint64_t>(b.ramTotal / 8, 4ull << 30);
        uint64_t pool = cfg.vramPoolBytes;
        if (pool == 0) pool = (uint64_t)((double)(b.vramBudget > b.vramUsage ? b.vramBudget - b.vramUsage : 0) * cfg.poolFraction);
        const uint64_t heapBytes = (uint64_t)cfg.slotBytes * cfg.heapSlots;
        const uint32_t count = (uint32_t)std::max<uint64_t>(pool / heapBytes, 1);
        for (uint32_t h = 0; h < count; ++h) addHeap(heapBytes);
        logf("streaming: VRAM pool %u heaps x %.0f MB (budget %.0f MB, usage %.0f MB), RAM cache %.0f MB, upload <= %.0f MB/frame\n", count, heapBytes / 1048576.0,
             b.vramBudget / 1048576.0, b.vramUsage / 1048576.0, cfg.ramCacheBytes / 1048576.0, cfg.uploadBytesPerFrame / 1048576.0);
    }

    ~Impl()
    {
        drain();
    }

    void addHeap(uint64_t bytes)
    {
        Heap h;
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = bytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        check(device.d3d()->CreateHeap(&hd, IID_PPV_ARGS(&h.heap)), "streaming pool heap");
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.d3d()->CreatePlacedResource(h.heap.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&h.buffer)), "streaming pool buffer");
        h.buffer->SetName(L"unx streaming pool");
        heaps.push_back(std::move(h));
        slots.resize(slots.size() + cfg.heapSlots);
    }

    void drain()
    {
        if (fileQueue && readValue && readFence->GetCompletedValue() < readValue)
        {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            readFence->SetEventOnCompletion(readValue, e);
            WaitForSingleObject(e, INFINITE);
            CloseHandle(e);
        }
        if (memoryQueue && uploadValue && uploadFence->GetCompletedValue() < uploadValue)
        {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uploadFence->SetEventOnCompletion(uploadValue, e);
            WaitForSingleObject(e, INFINITE);
            CloseHandle(e);
        }
    }

    uint32_t heapOf(uint32_t slot) const { return slot / cfg.heapSlots; }

    void touchRam(uint64_t key)
    {
        auto it = ram.find(key);
        if (it != ram.end()) ramOrder.splice(ramOrder.end(), ramOrder, it->second.order);
    }

    // Frees RAM for 'bytes': least recently requested entries first, never a pinned or unfinished one, never one
    // requested this frame.
    bool reserveRam(uint64_t bytes)
    {
        for (auto it = ramOrder.begin(); ramUsed + bytes > cfg.ramCacheBytes && it != ramOrder.end();)
        {
            Ram& r = ram[*it];
            if (r.pins == 0 && r.ready && !requests.count(*it))
            {
                ramUsed -= r.bytes.size();
                ram.erase(*it);
                it = ramOrder.erase(it);
            }
            else
                ++it;
        }
        return ramUsed + bytes <= cfg.ramCacheBytes;
    }

    // A slot for a new page: a free one in a usable heap, else the least recently requested one whose page nobody
    // requested in the last framesInFlight + 1 frames (so no recorded frame reads it).
    uint32_t takeSlot()
    {
        uint32_t best = kNone;
        uint64_t bestFrame = UINT64_MAX;
        for (uint32_t s = 0; s < (uint32_t)slots.size(); ++s)
        {
            const Heap& h = heaps[heapOf(s)];
            if (!h.resident || h.draining) continue;
            const Slot& sl = slots[s];
            if (sl.pending) continue;
            if (sl.key == UINT64_MAX) return s;
            if (requests.count(sl.key)) continue;
            if (sl.lastRequested + cfg.framesInFlight + 1 > frame) continue;
            if (sl.lastRequested < bestFrame)
            {
                bestFrame = sl.lastRequested;
                best = s;
            }
        }
        if (best != kNone)
        {
            Page& old = pages[slots[best].key];
            old.slot = kNone;
            slots[best].key = UINT64_MAX;
            ++stats.evictedPages;
        }
        return best;
    }

    void dropHeapPages(uint32_t h)
    {
        for (uint32_t s = h * cfg.heapSlots; s < (h + 1) * cfg.heapSlots; ++s)
            if (slots[s].key != UINT64_MAX && !slots[s].pending)
            {
                pages[slots[s].key].slot = kNone;
                slots[s].key = UINT64_MAX;
                ++stats.evictedPages;
            }
    }

    // Budget: heaps beyond what the budget allows drain (their pages stop being resident at once, so consumers stop
    // reading them this frame) and are evicted framesInFlight + 1 frames later; heaps come back (MakeResident) when the
    // budget allows them again.
    void applyBudget()
    {
        const uint64_t heapBytes = (uint64_t)cfg.slotBytes * cfg.heapSlots;
        uint32_t allowed;
        if (overrideBudget) allowed = (uint32_t)std::max<uint64_t>(overrideBudget / heapBytes, 1);
        else
        {
            const Budget b = queryBudget(device);
            uint64_t ours = 0;
            for (const Heap& h : heaps) ours += h.resident ? heapBytes : 0;
            const uint64_t others = b.vramUsage > ours ? b.vramUsage - ours : 0;
            const uint64_t room = b.vramBudget > others ? b.vramBudget - others : 0;
            allowed = (uint32_t)std::max<uint64_t>(room / heapBytes, 1);
        }
        for (uint32_t h = 0; h < (uint32_t)heaps.size(); ++h)
        {
            Heap& hp = heaps[h];
            const bool keep = h < allowed;
            if (!keep && hp.resident && !hp.draining)
            {
                hp.draining = true;
                hp.drainFrame = frame;
                dropHeapPages(h);
            }
            if (hp.draining && keep)
                hp.draining = false;  // budget back before the eviction: keep it
            if (hp.draining && frame >= hp.drainFrame + cfg.framesInFlight + 1)
            {
                bool busy = false;
                for (uint32_t s = h * cfg.heapSlots; s < (h + 1) * cfg.heapSlots; ++s) busy |= slots[s].pending;
                if (!busy)
                {
                    dropHeapPages(h);
                    ID3D12Pageable* p = hp.heap.Get();
                    check(device.d3d()->Evict(1, &p), "streaming: Evict pool heap");
                    hp.resident = false;
                    hp.draining = false;
                }
            }
            if (!hp.resident && keep)
            {
                ID3D12Pageable* p = hp.heap.Get();
                check(device.d3d()->MakeResident(1, &p), "streaming: MakeResident pool heap");
                hp.resident = true;
            }
        }
        stats.heapsResident = stats.heapsEvicted = 0;
        for (const Heap& h : heaps) (h.resident ? stats.heapsResident : stats.heapsEvicted) += 1;
    }

    void complete()
    {
        const uint64_t reads = readFence->GetCompletedValue(), uploads = uploadFence->GetCompletedValue();
        std::vector<uint64_t> bad;
        for (auto& [key, r] : ram)
            if (!r.ready && r.readValue <= reads)
            {
                r.ready = true;
                if (cfg.verifyPages && fnv1a(r.bytes.data(), r.bytes.size()) != files[key >> 32].table[(uint32_t)key].fnv) bad.push_back(key);
            }
        for (uint64_t key : bad)  // never made resident; a later request reads it again
        {
            ++stats.hashFailures;
            ramUsed -= ram[key].bytes.size();
            ramOrder.erase(ram[key].order);
            ram.erase(key);
        }
        for (auto& [key, p] : pages)
            if (p.pendingSlot != kNone && p.uploadValue <= uploads)
            {
                slots[p.pendingSlot].pending = false;
                p.slot = p.pendingSlot;
                p.pendingSlot = kNone;
                auto it = ram.find(key);
                if (it != ram.end() && it->second.pins) --it->second.pins;
            }
    }

    void update(uint64_t frameIndex)
    {
        frame = frameIndex;
        Stats reset;
        reset.hashFailures = stats.hashFailures;  // cumulative
        stats = reset;
        complete();
        applyBudget();
        std::vector<std::pair<float, uint64_t>> order;
        order.reserve(requests.size());
        for (const auto& [key, priority] : requests) order.push_back({ priority, key });
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        uint64_t uploadBudget = cfg.uploadBytesPerFrame, readBudget = cfg.uploadBytesPerFrame * 2;
        bool readsIssued = false, uploadsIssued = false;
        for (const auto& [priority, key] : order)
        {
            (void)priority;
            Page& p = pages[key];
            p.lastRequested = frame;
            ++stats.requested;
            touchRam(key);
            if (p.slot != kNone)
            {
                slots[p.slot].lastRequested = frame;
                ++stats.residentRequested;
                continue;
            }
            if (p.pendingSlot != kNone) continue;
            const File& f = files[key >> 32];
            const PageEntry& e = f.table[(uint32_t)key];
            auto r = ram.find(key);
            if (r != ram.end())
            {
                if (!r->second.ready) continue;  // its read is in flight
                if (e.bytes > uploadBudget) continue;
                const uint32_t slot = takeSlot();
                if (slot == kNone) continue;  // every slot holds a page requested recently (budget full)
                DSTORAGE_REQUEST q{};
                q.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                q.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
                q.Source.Memory.Source = r->second.bytes.data();
                q.Source.Memory.Size = e.bytes;
                q.Destination.Buffer.Resource = heaps[heapOf(slot)].buffer.Get();
                q.Destination.Buffer.Offset = (uint64_t)(slot % cfg.heapSlots) * cfg.slotBytes;
                q.Destination.Buffer.Size = e.bytes;
                q.UncompressedSize = e.bytes;
                memoryQueue->EnqueueRequest(&q);
                slots[slot].key = key;
                slots[slot].pending = true;
                slots[slot].lastRequested = frame;
                p.pendingSlot = slot;
                p.uploadValue = uploadValue + 1;
                ++r->second.pins;
                uploadBudget -= e.bytes;
                ++stats.uploads;
                ++stats.ramHits;
                stats.uploadBytes += e.bytes;
                uploadsIssued = true;
                continue;
            }
            if (e.bytes > readBudget || !reserveRam(e.bytes)) continue;
            Ram& fresh = ram[key];
            fresh.bytes.resize(e.bytes);
            fresh.readValue = readValue + 1;
            ramOrder.push_back(key);
            fresh.order = std::prev(ramOrder.end());
            ramUsed += e.bytes;
            DSTORAGE_REQUEST q{};
            q.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            q.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            q.Source.File.Source = f.handle.Get();
            q.Source.File.Offset = e.offset;
            q.Source.File.Size = e.bytes;
            q.Destination.Memory.Buffer = fresh.bytes.data();
            q.Destination.Memory.Size = e.bytes;
            q.UncompressedSize = e.bytes;
            fileQueue->EnqueueRequest(&q);
            readBudget -= e.bytes;
            ++stats.diskReads;
            stats.diskBytes += e.bytes;
            readsIssued = true;
        }
        if (readsIssued)
        {
            fileQueue->EnqueueSignal(readFence.Get(), ++readValue);
            fileQueue->Submit();
        }
        if (uploadsIssued)
        {
            memoryQueue->EnqueueSignal(uploadFence.Get(), ++uploadValue);
            memoryQueue->Submit();
        }
        requests.clear();
        for (const auto& [key, p] : pages) stats.resident += p.slot != kNone;
        stats.ramCacheUsed = ramUsed;
        stats.readsIssued = readValue;
        stats.readsCompleted = readFence->GetCompletedValue();
        stats.uploadsIssued = uploadValue;
        stats.uploadsCompleted = uploadFence->GetCompletedValue();
        DSTORAGE_ERROR_RECORD err{};
        fileQueue->RetrieveErrorRecord(&err);
        if (FAILED(err.FirstFailure.HResult)) fail("streaming: DirectStorage read failed (0x%08x)", (unsigned)err.FirstFailure.HResult);
        memoryQueue->RetrieveErrorRecord(&err);
        if (FAILED(err.FirstFailure.HResult)) fail("streaming: DirectStorage upload failed (0x%08x)", (unsigned)err.FirstFailure.HResult);
    }
};

Streamer::Streamer(render::Device& device, const Settings& settings) : m(std::make_unique<Impl>(device, settings)) {}
Streamer::~Streamer() = default;

uint32_t Streamer::addFile(const std::string& path)
{
    Impl::File f;
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("streaming: cannot open %s", path.c_str());
    uint32_t head[4];
    in.read((char*)head, 16);
    if (!in || head[0] != kMagic || head[1] != kVersion) fail("streaming: %s is not a page file (version %u)", path.c_str(), kVersion);
    f.table.resize(head[2]);
    in.read((char*)f.table.data(), (std::streamsize)(f.table.size() * sizeof(PageEntry)));
    if (!in) fail("streaming: %s: truncated page table", path.c_str());
    for (const PageEntry& e : f.table)
        if (e.bytes > m->cfg.slotBytes) fail("streaming: %s has a page of %u B, the slot holds %u B", path.c_str(), e.bytes, m->cfg.slotBytes);
    const std::wstring wide = std::filesystem::path(path).wstring();
    check(m->factory->OpenFile(wide.c_str(), IID_PPV_ARGS(&f.handle)), "DirectStorage OpenFile");
    m->files.push_back(std::move(f));
    return (uint32_t)m->files.size() - 1;
}

uint32_t Streamer::pageCount(uint32_t file) const { return (uint32_t)m->files.at(file).table.size(); }
uint32_t Streamer::pageBytes(uint32_t file, uint32_t page) const { return m->files.at(file).table.at(page).bytes; }

void Streamer::request(uint32_t file, uint32_t page, float priority)
{
    if (file >= m->files.size() || page >= m->files[file].table.size()) fail("streaming: request of page %u of file %u", page, file);
    auto [it, added] = m->requests.try_emplace(keyOf(file, page), priority);
    if (!added) it->second = std::max(it->second, priority);
}

void Streamer::update(uint64_t frameIndex) { m->update(frameIndex); }

void Streamer::flush()
{
    m->drain();
    m->complete();
}

uint32_t Streamer::residentSlot(uint32_t file, uint32_t page) const
{
    auto it = m->pages.find(keyOf(file, page));
    return it == m->pages.end() ? kNone : it->second.slot;
}

uint32_t Streamer::heapCount() const { return (uint32_t)m->heaps.size(); }
ID3D12Resource* Streamer::heapBuffer(uint32_t heap) const { return m->heaps.at(heap).buffer.Get(); }
bool Streamer::heapResident(uint32_t heap) const { return m->heaps.at(heap).resident; }
const Settings& Streamer::settings() const { return m->cfg; }
const Stats& Streamer::stats() const { return m->stats; }
uint64_t Streamer::poolBytes() const { return (uint64_t)m->cfg.slotBytes * m->cfg.heapSlots * m->heaps.size(); }
void Streamer::overridePoolBudget(uint64_t bytes) { m->overrideBudget = bytes; }
} // namespace unx::streaming
