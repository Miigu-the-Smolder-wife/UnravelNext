#include "unx/render/GpuProfiler.h"

#include <algorithm>

namespace unx::render
{
GpuProfiler::GpuProfiler(Device& device, uint32_t framesInFlight, uint32_t maxPassesPerFrame)
    : m_device(device), m_framesInFlight(framesInFlight), m_perQueue(2 * maxPassesPerFrame + 8)
{
    const uint32_t queues = 2;  // graphics and compute (the copy queue needs its own query type and has no passes)
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = m_perQueue * queues * framesInFlight;
    check(device.d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_heap)), "CreateQueryHeap(TIMESTAMP)");
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)qd.Count * sizeof(uint64_t);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_readback)), "timestamp readback buffer");
    D3D12_RANGE none{ 0, 0 };
    check(m_readback->Map(0, &none, reinterpret_cast<void**>(&m_mapped)), "Map timestamp readback");
    m_slots.resize(framesInFlight);

    LARGE_INTEGER qpcFrequency;
    QueryPerformanceFrequency(&qpcFrequency);
    for (uint32_t q = 0; q < queues; ++q)
    {
        Queue& queue = device.queue((QueueType)q);
        uint64_t gpu = 0, cpu = 0;
        check(queue.get()->GetClockCalibration(&gpu, &cpu), "GetClockCalibration");
        m_msPerTick[q] = 1000.0 / (double)queue.timestampFrequency();
        m_calibrationOffsetMs[q] = (double)cpu * 1000.0 / (double)qpcFrequency.QuadPart - (double)gpu * m_msPerTick[q];
    }
}

GpuProfiler::~GpuProfiler()
{
    if (m_readback) m_readback->Unmap(0, nullptr);
}

void GpuProfiler::beginFrame(uint64_t frame)
{
    Slot& slot = m_slots[frame % m_framesInFlight];
    if (slot.frame != UINT64_MAX) read(slot);
    slot.frame = frame;
    slot.events.clear();
    for (uint32_t q = 0; q < kQueueTypeCount; ++q)
    {
        slot.used[q] = 0;
        slot.lists[q].clear();
        m_open[q].clear();
        m_lastMark[q] = UINT32_MAX;
    }
    m_current = &slot;
}

uint32_t GpuProfiler::allocate(QueueType queue)
{
    uint32_t q = (uint32_t)queue;
    if (queue == QueueType::Copy) fail("timestamps on the copy queue are not supported");
    uint32_t& used = m_current->used[q];
    if (used >= m_perQueue) fail("GpuProfiler: more than %u timestamps on the %s queue in one frame", m_perQueue, queueName(queue));
    uint32_t slotIndex = (uint32_t)(m_current - m_slots.data());
    return (slotIndex * 2 + q) * m_perQueue + used++;
}

void GpuProfiler::listBegin(ID3D12GraphicsCommandList* cmd, QueueType queue)
{
    uint32_t i = allocate(queue);
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
    m_current->lists[(size_t)queue].push_back({ i, UINT32_MAX, i });
    m_lastMark[(size_t)queue] = i;
}

void GpuProfiler::listEnd(ID3D12GraphicsCommandList* cmd, QueueType queue)
{
    auto& lists = m_current->lists[(size_t)queue];
    if (lists.empty() || lists.back().end != UINT32_MAX) fail("GpuProfiler::listEnd without listBegin");
    lists.back().lastPassEnd = m_lastMark[(size_t)queue];
    uint32_t i = allocate(queue);
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
    lists.back().end = i;
    m_lastMark[(size_t)queue] = i;
}

void GpuProfiler::passBegin(ID3D12GraphicsCommandList* cmd, QueueType queue, std::string_view name)
{
    if (!m_passTimestamps) return;
    uint32_t begin = m_lastMark[(size_t)queue];
    if (begin == UINT32_MAX)
    {
        begin = allocate(queue);  // no mark yet on this queue in this command list sequence
        cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, begin);
    }
    m_current->events.push_back({ std::string(name), queue, begin, UINT32_MAX });
    m_open[(size_t)queue].push_back((uint32_t)m_current->events.size() - 1);
}

void GpuProfiler::passEnd(ID3D12GraphicsCommandList* cmd, QueueType queue)
{
    if (!m_passTimestamps) return;
    auto& open = m_open[(size_t)queue];
    if (open.empty()) fail("GpuProfiler::passEnd without passBegin");
    uint32_t i = allocate(queue);
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
    m_current->events[open.back()].end = i;
    open.pop_back();
    m_lastMark[(size_t)queue] = i;
}

void GpuProfiler::resolve(ID3D12GraphicsCommandList* cmd, QueueType queue)
{
    uint32_t q = (uint32_t)queue;
    uint32_t count = m_current->used[q];
    if (count == 0) return;
    uint32_t slotIndex = (uint32_t)(m_current - m_slots.data());
    uint32_t first = (slotIndex * 2 + q) * m_perQueue;
    cmd->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, m_readback.Get(), (UINT64)first * sizeof(uint64_t));
}

void GpuProfiler::read(Slot& slot)
{
    auto toMs = [&](QueueType q, uint32_t index) { return (double)m_mapped[index] * m_msPerTick[(size_t)q] + m_calibrationOffsetMs[(size_t)q]; };
    double first = 1e300, last = -1e300;
    for (uint32_t q = 0; q < 2; ++q)
        for (const ListMarks& l : slot.lists[q])
            for (uint32_t i : { l.begin, l.end })
            {
                if (i == UINT32_MAX) continue;
                double t = toMs((QueueType)q, i);
                first = std::min(first, t);
                last = std::max(last, t);
            }
    for (const Event& e : slot.events)
    {
        if (e.end == UINT32_MAX) continue;
        first = std::min(first, toMs(e.queue, e.begin));
        last = std::max(last, toMs(e.queue, e.end));
    }
    m_completed.frame = slot.frame;
    m_completed.gpuFrameMs = last > first ? last - first : 0;
    m_completed.passes.clear();
    for (const Event& e : slot.events)
    {
        if (e.end == UINT32_MAX) continue;
        m_completed.passes.push_back({ e.name, e.queue, toMs(e.queue, e.begin) - first, toMs(e.queue, e.end) - first });
    }
    for (uint32_t q = 0; q < 2; ++q)
    {
        QueueTiming& qt = m_completed.queues[q];
        qt = QueueTiming{};
        const auto& lists = slot.lists[q];
        qt.lists = (uint32_t)lists.size();
        for (size_t k = 0; k < lists.size(); ++k)
        {
            const ListMarks& l = lists[k];
            if (l.end == UINT32_MAX) continue;
            if (k == 0) qt.headMs = std::max(toMs((QueueType)q, l.begin) - first, 0.0);
            qt.tailMs += std::max(toMs((QueueType)q, l.end) - toMs((QueueType)q, l.lastPassEnd), 0.0);
            if (k > 0 && lists[k - 1].end != UINT32_MAX) qt.gapMs += std::max(toMs((QueueType)q, l.begin) - toMs((QueueType)q, lists[k - 1].end), 0.0);
        }
    }
    m_hasCompleted = true;
}
} // namespace unx::render
