// Projected decals (track E, A7). See include/unx/decal/Decals.h and Decal.hlsli.
#include "unx/decal/Decals.h"

#include "unx/core/Log.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace unx::decal
{
using namespace unx::render;

namespace
{
constexpr uint32_t kRecordBytes = 128, kFrameBytes = 160, kTilePx = 16, kTileWords = 9, kTilesHeader = 16;

struct Record  // Decal.hlsli DecalRecord
{
    float box[3][4];
    uint32_t material, instance;
    int32_t priority;
    uint32_t order;
    float opacity, cosFadeStart, cosFadeEnd, edge;
    float color[3];
    uint32_t channels;
    float fadeScreenSize, fadeInStart, fadeInDuration, fadeOutStart;
    float fadeOutDuration, emissive, pad[2];
};
static_assert(sizeof(Record) == kRecordBytes);

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "decal buffer");
    r->SetName(name);
    return r;
}

class DecalPass
{
public:
    explicit DecalPass(Device& device) : m_device(device) {}
    ~DecalPass()
    {
        m_device.waitIdle();
        for (Slot& s : m_slots)
        {
            if (s.upload && s.uploadMapped) s.upload->Unmap(0, nullptr);
            if (s.readback && s.readbackMapped) s.readback->Unmap(0, nullptr);
        }
    }

    void record(FramePassContext& fc, ViewResources& view, const DecalSet& set)
    {
        const uint32_t capacity = (uint32_t)fc.quality.integer("decal.max_decals");
        if (capacity == 0 || capacity > 65535) fail("decal.max_decals must be in [1, 65535] (16-bit tile entries)");
        ensureSlots(fc.framesInFlight);
        const uint64_t frame = fc.frame.frameIndex;
        collectStats(frame);
        const uint32_t count = set.count();
        if (count == 0 || !view.depth.valid()) return;
        if (count > capacity) fail("decals: %u live decals exceed decal.max_decals = %u", count, capacity);
        if (!m_records || m_capacity != capacity)
        {
            if (m_records) m_device.deferRelease(m_records);
            m_capacity = capacity;
            m_records = makeBuffer(m_device, (uint64_t)capacity * kRecordBytes, D3D12_HEAP_TYPE_DEFAULT, L"decal records");
            m_uploadedRevision = 0;
        }
        RenderGraph& g = fc.graph;
        const BufferRef records = g.importBuffer(m_records.Get(), BufferDesc{ "decal.records", (uint64_t)capacity * kRecordBytes, kRecordBytes });
        Slot& slot = m_slots[frame % m_slots.size()];
        if (set.revision() != m_uploadedRevision)
        {
            const std::vector<uint8_t> bytes = set.records();
            ensureUpload(slot, bytes.size());
            std::memcpy(slot.uploadMapped, bytes.data(), bytes.size());
            ID3D12Resource* upload = slot.upload.Get();
            const uint64_t size = bytes.size();
            g.addPass("decal.upload", QueueType::Graphics, [=](PassBuilder& b) { b.use(records, Use::CopyDst); },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(records), 0, upload, 0, size); });
            m_uploadedRevision = set.revision();
        }

        const uint32_t W = view.view.width, H = view.view.height;
        const uint32_t tilesX = (W + kTilePx - 1) / kTilePx, tilesY = (H + kTilePx - 1) / kTilePx, tileCount = tilesX * tilesY;
        const BufferRef frames = g.createBuffer(BufferDesc{ "decal.frames", (uint64_t)count * kFrameBytes, kFrameBytes });
        const BufferRef tiles = g.createBuffer(BufferDesc{ "decal.tiles", kTilesHeader + (uint64_t)tileCount * kTileWords * 4, 0 });
        const BufferRef tileDepth = g.createBuffer(BufferDesc{ "decal.tileDepth", (uint64_t)tileCount * 8, 8 });
        const TextureRef depth = view.depth;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
        auto dispatch = [&](const char* name, uint32_t step, uint32_t gx, uint32_t gy, std::function<void(PassBuilder&)> uses) {
            ID3D12PipelineState* pso = fc.shaders.compute(std::string("Passes/Decal/DecalSetup.STEP") + std::to_string(step));
            g.addPass(name, QueueType::Graphics, uses, [=](PassContext& c) {
                const uint32_t k[8] = { c.srv(records), c.uav(frames), c.uav(tiles), count, c.srv(depth), c.uav(tileDepth), tilesX, tilesY };
                c.cmd->SetPipelineState(pso);
                c.bindFrameConstants(constants);
                c.computeConstants(k, 8);
                c.cmd->Dispatch(gx, gy, 1);
            });
        };
        auto all = [=](Use recordsUse, Use framesUse, Use tilesUse, Use depthUse) {
            return [=](PassBuilder& b) {
                b.use(records, recordsUse);
                b.use(frames, framesUse);
                b.use(tiles, tilesUse);
                b.use(depth, Use::SrvCompute);
                b.use(tileDepth, depthUse);
            };
        };
        // One dimension: tiles / 64 (507 groups at 4K) and decals (<= 65535, decal.max_decals); tile depth: one group per tile.
        if ((tileCount + 63) / 64 > 65535) fail("decals: %u tiles exceed one dispatch dimension", tileCount);
        dispatch("decal.clear", 0, (tileCount + 63) / 64, 1, all(Use::SrvCompute, Use::UavCompute, Use::UavCompute, Use::UavCompute));
        dispatch("decal.setup", 1, (count + 63) / 64, 1, all(Use::SrvCompute, Use::UavCompute, Use::UavCompute, Use::UavCompute));
        dispatch("decal.tileDepth", 2, tilesX, tilesY, all(Use::SrvCompute, Use::SrvCompute, Use::UavCompute, Use::UavCompute));
        dispatch("decal.cull", 3, count, 1, all(Use::SrvCompute, Use::SrvCompute, Use::UavCompute, Use::SrvCompute));
        view.decalFrames = frames;
        view.decalTiles = tiles;
        view.decalEmissive = set.anyEmissive();

        slot.readbackFrame = frame;
        slot.readbackDecals = count;
        ID3D12Resource* readback = slot.readback.Get();
        g.addPass("decal.stats", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(tiles, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, 0, c.resource(tiles), 0, 16); });
    }
    Stats stats() const { return m_stats; }

private:
    struct Slot
    {
        ComPtr<ID3D12Resource> upload, readback;
        uint8_t* uploadMapped = nullptr;
        uint8_t* readbackMapped = nullptr;
        uint64_t uploadBytes = 0;
        uint64_t readbackFrame = UINT64_MAX;
        uint32_t readbackDecals = 0;
    };
    void ensureSlots(uint32_t framesInFlight)
    {
        const size_t n = std::max(framesInFlight, 1u);
        if (m_slots.size() == n) return;
        m_device.waitIdle();
        for (Slot& s : m_slots)
        {
            if (s.upload && s.uploadMapped) s.upload->Unmap(0, nullptr);
            if (s.readback && s.readbackMapped) s.readback->Unmap(0, nullptr);
        }
        m_slots.clear();
        m_slots.resize(n);
        for (Slot& s : m_slots)
        {
            s.readback = makeBuffer(m_device, 16, D3D12_HEAP_TYPE_READBACK, L"decal stats readback");
            D3D12_RANGE all{ 0, 16 };
            check(s.readback->Map(0, &all, reinterpret_cast<void**>(&s.readbackMapped)), "map decal stats");
        }
    }
    void collectStats(uint64_t frame)
    {
        Slot& s = m_slots[frame % m_slots.size()];
        if (s.readbackFrame == UINT64_MAX) return;
        uint32_t h[4];
        std::memcpy(h, s.readbackMapped, sizeof h);
        if (m_stats.frame == UINT64_MAX || s.readbackFrame > m_stats.frame) m_stats = { s.readbackFrame, s.readbackDecals, h[3] };
        s.readbackFrame = UINT64_MAX;
    }
    void ensureUpload(Slot& s, uint64_t bytes)
    {
        if (s.upload && s.uploadBytes >= bytes) return;
        if (s.upload)
        {
            s.upload->Unmap(0, nullptr);
            m_device.deferRelease(s.upload);
        }
        s.uploadBytes = std::max<uint64_t>(bytes, 16 * 1024);
        s.upload = makeBuffer(m_device, s.uploadBytes, D3D12_HEAP_TYPE_UPLOAD, L"decal records upload");
        D3D12_RANGE none{ 0, 0 };
        check(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.uploadMapped)), "map decal upload");
    }

    Device& m_device;
    ComPtr<ID3D12Resource> m_records;
    uint32_t m_capacity = 0;
    uint64_t m_uploadedRevision = 0;
    std::vector<Slot> m_slots;
    Stats m_stats;
};
} // namespace

uint32_t DecalSet::add(const Decal& d)
{
    uint32_t id;
    if (!m_free.empty())
    {
        id = m_free.back();
        m_free.pop_back();
    }
    else
    {
        id = (uint32_t)m_slots.size();
        m_slots.emplace_back();
    }
    m_slots[id] = { d, m_nextOrder++, true };
    ++m_live;
    ++m_revision;
    return id;
}
void DecalSet::update(uint32_t id, const Decal& d)
{
    if (id >= m_slots.size() || !m_slots[id].live) fail("DecalSet::update: no decal %u", id);
    m_slots[id].decal = d;
    ++m_revision;
}
void DecalSet::remove(uint32_t id)
{
    if (id >= m_slots.size() || !m_slots[id].live) fail("DecalSet::remove: no decal %u", id);
    m_slots[id].live = false;
    m_free.push_back(id);
    --m_live;
    ++m_revision;
}
void DecalSet::clear()
{
    m_slots.clear();
    m_free.clear();
    m_live = 0;
    ++m_revision;
}
uint32_t DecalSet::channelWord(const Decal& d)
{
    switch (d.blend)
    {
    case DecalBlend::Stain: return d.channels | 0x100u;  // Decal.hlsli DECAL_STAIN
    case DecalBlend::Normal: return DecalNormal;
    case DecalBlend::Emissive: return DecalEmissive;
    default: return d.channels;
    }
}
bool DecalSet::anyEmissive() const
{
    for (const Slot& s : m_slots)
        if (s.live && (channelWord(s.decal) & DecalEmissive) != 0) return true;
    return false;
}
std::vector<uint8_t> DecalSet::records() const
{
    std::vector<uint8_t> out;
    out.reserve((size_t)m_live * sizeof(Record));
    for (const Slot& s : m_slots)
    {
        if (!s.live) continue;
        const Decal& d = s.decal;
        if (!(d.fadeStartDegrees >= 0 && d.fadeStartDegrees < d.fadeEndDegrees && d.fadeEndDegrees <= 180) || !(d.opacity >= 0 && d.opacity <= 1) ||
            !(d.edge >= 0 && d.edge <= 1))
            fail("decal: fade 0 <= start < end <= 180 degrees, opacity and edge in [0, 1]");
        for (float v : { d.color.x, d.color.y, d.color.z, d.fadeScreenSize, d.fadeInDuration, d.fadeOutDuration })
            if (!std::isfinite(v) || v < 0) fail("decal: colour, screen-size fade and fade durations are finite and not negative");
        if (!std::isfinite(d.fadeInStart) || !std::isfinite(d.fadeOutStart)) fail("decal: fade start times are finite");
        if (d.channels == 0 || d.channels > (DecalAllChannels | DecalEmissive)) fail("decal: channels %u (1..15: DecalChannels)", d.channels);
        if (!std::isfinite(d.emissive) || d.emissive < 0 || (uint32_t)d.blend > 3) fail("decal: emissive scale >= 0, blend a DecalBlend");
        Record r{};
        std::memcpy(r.box, d.box.m, sizeof r.box);
        r.material = d.material;
        r.instance = d.instance;
        r.priority = d.priority;
        r.order = s.order;
        r.opacity = d.opacity;
        r.cosFadeStart = std::cos(d.fadeStartDegrees * 3.14159265358979f / 180.0f);
        r.cosFadeEnd = std::cos(d.fadeEndDegrees * 3.14159265358979f / 180.0f);
        r.edge = d.edge;
        r.color[0] = d.color.x, r.color[1] = d.color.y, r.color[2] = d.color.z;
        r.channels = channelWord(d);
        r.emissive = d.emissive;
        r.fadeScreenSize = d.fadeScreenSize;
        r.fadeInStart = d.fadeInStart, r.fadeInDuration = d.fadeInDuration;
        r.fadeOutStart = d.fadeOutStart, r.fadeOutDuration = d.fadeOutDuration;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&r);
        out.insert(out.end(), p, p + sizeof r);
    }
    return out;
}

DecalSet& decals(TrackState& state) { return state.get<DecalSet>("decal.set"); }
Stats lastStats(TrackState& state)
{
    auto& p = state.get<std::unique_ptr<DecalPass>>("decal.pass");
    return p ? p->stats() : Stats{};
}
} // namespace unx::decal

namespace unx::render::tracks
{
void decals(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState) return;
    auto& p = fc.trackState->get<std::unique_ptr<decal::DecalPass>>("decal.pass");
    if (!p) p = std::make_unique<decal::DecalPass>(fc.device);
    p->record(fc, view, decal::decals(*fc.trackState));
}
} // namespace unx::render::tracks
