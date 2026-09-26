#pragma once
// Three-tier streaming (render C, C6; ARCHITECTURE 2.13b, user rule 2026-09-26 "일반 PC에서도 같은 구조"): content pages
// live on NVMe in a page file, a bounded RAM cache keeps recently read pages, and VRAM holds the pages the frame needs
// in pools sized from the device's budget (DXGI QueryVideoMemoryInfo) and kept with standard D3D12 residency calls
// (ID3D12Device::Evict / MakeResident on the pool heaps when the budget moves). The same code runs on a 16 GB RAM / 8 GB
// VRAM PC and on the development PC; only the cache and pool sizes differ, never the content or its quality.
//
// Contract with consumers (geometry pages, texture mips, BLAS inputs): a consumer requests pages with a priority every
// frame and reads a page only when it is resident (residentSlot != kNone). A page is never replaced by a coarser
// substitute: "resident before use" is reached by requesting ahead (prefetch: distance, camera speed x latency), and the
// streamer guarantees that a requested page that fits the budget becomes resident within
//   ceil(bytes of higher-priority missing pages / uploadBytesPerFrame) + 2 frames
// (read and upload complete in the frame after they are issued; DirectStorage queues are asynchronous).
// Cost per frame [expected]: CPU O(requests log requests) for the priority order; copy traffic <= uploadBytesPerFrame
// (64 MB: 2.5 ms of PCIe 4 x16 on the DirectStorage copy queue, off the graphics queue); GPU frame cost 0.
//
// Slot reuse is safe against frames in flight: a slot is reused only when its page was not requested in the last
// framesInFlight + 1 frames, so no recorded frame can still read it.
#include "unx/render/Device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unx::streaming
{
constexpr uint32_t kNone = 0xFFFFFFFFu;

// Page file: header, page table (offset, bytes, SHA-256 of the bytes), pages aligned to 4 KB (DirectStorage and NVMe
// friendly). Written by the cook; read by the streamer, which checks each page's hash when it enters the RAM cache.
struct PageFileWriter
{
    static void write(const std::string& path, const std::vector<std::vector<uint8_t>>& pages);
};

struct Budget
{
    uint64_t vramBudget = 0, vramUsage = 0;  // DXGI local segment: budget and this process's usage
    uint64_t ramTotal = 0, ramAvailable = 0;
};
Budget queryBudget(render::Device& device);

struct Settings
{
    uint32_t slotBytes = 64 * 1024;           // a VRAM slot holds one page (pages larger than a slot are rejected)
    uint32_t heapSlots = 1024;                // slots per pool heap (64 MB heaps at 64 KB slots)
    uint64_t vramPoolBytes = 0;               // pool capacity; 0 = from the budget (poolFraction of the free budget)
    double poolFraction = 0.25;               // share of (budget - usage) taken when vramPoolBytes = 0
    uint64_t ramCacheBytes = 0;               // RAM cache; 0 = min(1/8 of physical RAM, 4 GB) (16 GB PC: 2 GB)
    uint64_t uploadBytesPerFrame = 64ull << 20;  // ARCHITECTURE 2.13b: <= 64 MB per frame
    uint32_t framesInFlight = 2;
    bool verifyPages = false;                 // check each page read against the page table's FNV-1a (tests)
};

struct Stats
{
    uint32_t requested = 0, resident = 0, residentRequested = 0;  // this frame
    uint32_t diskReads = 0, ramHits = 0, uploads = 0;            // issued this frame
    uint64_t uploadBytes = 0, diskBytes = 0;
    uint32_t evictedPages = 0;                                   // slots reclaimed this frame
    uint32_t hashFailures = 0;                                   // cumulative: pages whose bytes did not match (never made resident)
    uint32_t heapsResident = 0, heapsEvicted = 0;
    uint64_t ramCacheUsed = 0;
    uint64_t readsIssued = 0, readsCompleted = 0, uploadsIssued = 0, uploadsCompleted = 0;  // DirectStorage fence values
};

class Streamer
{
public:
    Streamer(render::Device& device, const Settings& settings);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    uint32_t addFile(const std::string& path);  // returns the file id
    uint32_t pageCount(uint32_t file) const;
    uint32_t pageBytes(uint32_t file, uint32_t page) const;

    // This frame's needs: higher priority first (e.g. -distance, or screen size). Requests are forgotten after update().
    void request(uint32_t file, uint32_t page, float priority);
    // Completes finished reads and uploads, then issues new ones for the highest-priority missing pages within the
    // budgets, reclaiming slots of pages no longer requested (older than framesInFlight + 1 frames) and, when the VRAM
    // budget shrank below the pool, evicting whole heaps (their pages dropped first). Call once per frame, before the
    // frame's consumers read residentSlot().
    void update(uint64_t frameIndex);
    // Blocks until every issued read and upload completed and applies them (tests, loading screens).
    void flush();

    uint32_t residentSlot(uint32_t file, uint32_t page) const;  // kNone when not resident
    // Pool heaps and their buffers: slot s lives in heap s / heapSlots at byte (s % heapSlots) * slotBytes.
    uint32_t heapCount() const;
    ID3D12Resource* heapBuffer(uint32_t heap) const;
    bool heapResident(uint32_t heap) const;  // false while evicted (no command list may reference it)
    const Settings& settings() const;
    const Stats& stats() const;
    uint64_t poolBytes() const;
    // Tests: replace the measured VRAM budget (bytes of pool the budget allows); 0 = measured.
    void overridePoolBudget(uint64_t bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace unx::streaming
