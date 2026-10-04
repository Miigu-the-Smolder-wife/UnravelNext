#pragma once
#include "unx/render/Device.h"

#include <string>
#include <string_view>
#include <vector>

namespace unx::render
{
struct PassTiming
{
    std::string name;
    QueueType queue = QueueType::Graphics;
    double beginMs = 0;  // relative to the frame's first timestamp, on a common (CPU-calibrated) timeline
    double endMs = 0;
    double durationMs() const { return endMs - beginMs; }
};

// Per queue, the frame time outside its passes (v1.39, I request 20260926_I_profiler_gaps.md). Passes tile each
// command list from its begin mark, so on one queue: frame span = head + passes + tails + gaps.
struct QueueTiming
{
    uint32_t lists = 0;   // command lists of the frame on this queue (list boundaries = lists - 1)
    double headMs = 0;    // the frame's first timestamp (any queue) to this queue's first list begin
    double tailMs = 0;    // sum over lists of last pass end to list end: the barriers closing each list (the frame's
                          // final layout transitions on the last list)
    double gapMs = 0;     // sum of list end to next list begin: time the queue ran other work or idled between lists
    double workMs = 0;    // sum of list begin to final work end, available without detailed pass timestamps
};

struct FrameTiming
{
    uint64_t frame = 0;
    double gpuFrameMs = 0;  // first to last timestamp over all queues
    uint32_t timestampCount = 0;
    bool detailedPassTimings = true;
    std::vector<PassTiming> passes;
    QueueTiming queues[2];  // graphics, compute
};

// GPU timestamps for every pass, on by default for direct users/Harness (the production Host opts out). One timestamp per pass boundary: a pass
// spans from the previous mark on its queue (frame begin or the previous pass's end) to its own end mark, so its
// time includes the barriers that prepare its inputs. (A begin/end pair per pass cost 0.26 us per pass [measured].)
// Each queue owns its own query index range per frame slot and resolves it at the end of its last command list;
// results are read back once the slot's fences have completed. Timestamps from different queues are placed on one
// timeline with each queue's clock calibration against QueryPerformanceCounter.
class GpuProfiler
{
public:
    GpuProfiler(Device& device, uint32_t framesInFlight, uint32_t maxPassesPerFrame);
    ~GpuProfiler();

    // Starts frame 'frame' in slot frame % framesInFlight. The caller has already waited for that slot's fences.
    // Returns the timing of the frame that previously used the slot (if any) through lastCompleted().
    void beginFrame(uint64_t frame);
    const FrameTiming* lastCompleted() const { return m_hasCompleted ? &m_completed : nullptr; }

    // Recording interface used by the render graph: every command list opens with listBegin and closes with listEnd
    // (after its last barriers); passes chain from the list's begin mark.
    void listBegin(ID3D12GraphicsCommandList* cmd, QueueType queue);
    void listEnd(ID3D12GraphicsCommandList* cmd, QueueType queue);
    // Frame/queue-only mode: one mark after the final actual pass, before list-closing barriers. Detailed mode already
    // has that mark from passEnd. This preserves tail/work attribution without recording every pass boundary.
    void listWorkEnd(ID3D12GraphicsCommandList* cmd, QueueType queue);
    void passBegin(ID3D12GraphicsCommandList* cmd, QueueType queue, std::string_view name);
    void passEnd(ID3D12GraphicsCommandList* cmd, QueueType queue);
    void resolve(ID3D12GraphicsCommandList* cmd, QueueType queue);  // once per queue per frame, last

    bool enabled() const { return m_current ? m_current->detailed : m_passTimestamps; }
    // Applies at the next beginFrame; already-recorded slots retain their own mode. Frame and queue markers stay.
    void setPassTimestamps(bool on) { m_passTimestamps = on; }

private:
    struct Event
    {
        std::string name;
        QueueType queue;
        uint32_t begin, end;
    };
    struct ListMarks
    {
        uint32_t begin, end, lastPassEnd;  // lastPassEnd = begin when the list has no pass
    };
    struct Slot
    {
        uint64_t frame = UINT64_MAX;
        bool detailed = true;
        std::vector<Event> events;
        uint32_t used[kQueueTypeCount] = {};
        std::vector<ListMarks> lists[kQueueTypeCount];
    };
    uint32_t allocate(QueueType queue);
    void read(Slot& slot);

    Device& m_device;
    uint32_t m_framesInFlight;
    uint32_t m_perQueue;  // query indices per queue per slot
    ComPtr<ID3D12QueryHeap> m_heap;
    // D3D12 tracks cross-queue writes at resource granularity, even for disjoint byte ranges.
    ComPtr<ID3D12Resource> m_readback[2];
    uint64_t* m_mapped[2] = {};
    std::vector<Slot> m_slots;
    Slot* m_current = nullptr;
    std::vector<uint32_t> m_open[kQueueTypeCount];  // stack of open events per queue
    uint32_t m_lastMark[kQueueTypeCount] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    double m_calibrationOffsetMs[kQueueTypeCount] = {};  // gpu tick -> ms on the CPU timeline
    double m_msPerTick[kQueueTypeCount] = {};
    bool m_passTimestamps = true;
    bool m_hasCompleted = false;
    FrameTiming m_completed;
};
} // namespace unx::render
