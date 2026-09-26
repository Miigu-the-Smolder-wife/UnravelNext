// Surface state field on the GPU (track E, A7). See include/unx/decal/SurfaceState.h and SurfaceState.hlsli.
#include "unx/decal/SurfaceState.h"

#include "unx/core/Log.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace unx::surface
{
using namespace unx::render;

uint32_t hash(int32_t x, int32_t y, int32_t z)
{
    auto round = [](uint32_t h) { return ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u; };
    uint32_t h = (uint32_t)z * 747796405u + 2891336453u;
    h = round(h);
    h ^= (uint32_t)y * 2654435761u;
    h = round(h);
    h ^= (uint32_t)x * 2246822519u;
    h = round(h);
    return (h >> 22u) ^ h;
}

void encode(const BrickInput& in, uint8_t out[kBrickBytes])
{
    std::memset(out, 0, kBrickBytes);
    std::memcpy(out, in.key, 12);
    const float t0 = (float)in.t0;
    std::memcpy(out + 12, &t0, 4);
    for (uint32_t v = 0; v < 64; ++v)
    {
        for (uint32_t c = 0; c < 5; ++c)
        {
            const float x = in.value[v * kChannels + c];
            out[16 + 5 * v + c] = (uint8_t)std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f);
        }
        const float snow = in.value[v * kChannels + 5];
        const int16_t s = (int16_t)std::clamp<long>(std::lround(std::clamp(snow, -16.0f, 16.0f) * 2048.0f), -32768, 32767);
        std::memcpy(out + 336 + 2 * v, &s, 2);
    }
}

void SurfaceField::setHalfLives(const std::array<double, kChannels>& halfLife)
{
    for (double h : halfLife)
        if (!(h >= 0) || !std::isfinite(h)) fail("surface: half-lives must be finite and >= 0");
    m_halfLife = halfLife;
}

void SurfaceField::clear()
{
    const uint32_t capacity = m_capacity;
    *this = SurfaceField{};
    if (capacity) setCapacity(capacity);
}

void SurfaceField::setCapacity(uint32_t maxBricks)
{
    if (maxBricks == m_capacity) return;
    if (maxBricks == 0) fail("surface.max_bricks must be positive");
    if (m_slotOf.size() > maxBricks) fail("surface: %zu bricks exceed surface.max_bricks = %u", m_slotOf.size(), maxBricks);
    // Rebuild: live bricks in slots 0..n-1, a fresh table.
    std::vector<std::pair<Key, std::vector<uint8_t>>> live;
    live.reserve(m_slotOf.size());
    for (const auto& [k, slot] : m_slotOf)
        live.push_back({ k, std::vector<uint8_t>(m_records.begin() + (size_t)slot * kBrickBytes, m_records.begin() + (size_t)(slot + 1) * kBrickBytes) });
    std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return std::tie(a.first.x, a.first.y, a.first.z) < std::tie(b.first.x, b.first.y, b.first.z); });
    m_capacity = maxBricks;
    uint32_t size = 1;
    while (size < 2 * maxBricks) size <<= 1;
    m_table.assign(size, { 0, 0, 0, (int32_t)kEmpty });
    m_records.assign((size_t)maxBricks * kBrickBytes, 0);
    m_slotOf.clear();
    m_free.clear();
    m_next = 0;
    m_maxProbe = 0;
    m_slotDirty.assign(maxBricks, 0);
    m_entryDirty.assign(size, 0);
    m_dirtySlots.clear();
    m_dirtyEntries.clear();
    for (auto& [k, record] : live)
    {
        const uint32_t slot = m_next++;
        std::memcpy(m_records.data() + (size_t)slot * kBrickBytes, record.data(), kBrickBytes);
        m_slotOf[k] = slot;
        insert(k, slot);
    }
    m_dirtySlots.clear();
    m_dirtyEntries.clear();
    std::fill(m_slotDirty.begin(), m_slotDirty.end(), 0);
    std::fill(m_entryDirty.begin(), m_entryDirty.end(), 0);
    m_whole = true;
}

void SurfaceField::dirtyEntry(uint32_t i)
{
    if (!m_entryDirty[i])
    {
        m_entryDirty[i] = 1;
        m_dirtyEntries.push_back(i);
    }
}

void SurfaceField::insert(const Key& k, uint32_t slot)
{
    const uint32_t mask = (uint32_t)m_table.size() - 1;
    uint32_t i = hash(k.x, k.y, k.z) & mask, d = 0;
    while ((uint32_t)m_table[i][3] != kEmpty)
    {
        if (m_table[i][0] == k.x && m_table[i][1] == k.y && m_table[i][2] == k.z) break;
        i = (i + 1) & mask;
        ++d;
    }
    m_table[i] = { k.x, k.y, k.z, (int32_t)slot };
    m_maxProbe = std::max(m_maxProbe, d);
    dirtyEntry(i);
}

void SurfaceField::erase(const Key& k)
{
    const uint32_t mask = (uint32_t)m_table.size() - 1;
    uint32_t i = hash(k.x, k.y, k.z) & mask;
    for (uint32_t p = 0; p <= m_maxProbe; ++p, i = (i + 1) & mask)
    {
        if ((uint32_t)m_table[i][3] == kEmpty) break;
        if (!(m_table[i][0] == k.x && m_table[i][1] == k.y && m_table[i][2] == k.z)) continue;
        // backward-shift deletion: later entries of the run move back while that keeps them at or after their home
        m_table[i][3] = (int32_t)kEmpty;
        dirtyEntry(i);
        uint32_t j = i;
        for (;;)
        {
            j = (j + 1) & mask;
            if ((uint32_t)m_table[j][3] == kEmpty) break;
            const uint32_t home = hash(m_table[j][0], m_table[j][1], m_table[j][2]) & mask;
            if (((j - home) & mask) >= ((j - i) & mask))
            {
                m_table[i] = m_table[j];
                m_table[j][3] = (int32_t)kEmpty;
                dirtyEntry(i);
                dirtyEntry(j);
                i = j;
            }
        }
        return;
    }
    fail("surface: removed brick (%d, %d, %d) is not in the table", k.x, k.y, k.z);
}

void SurfaceField::apply(const BrickInput* changed, size_t changedCount, const int32_t* removedKeys, size_t removedCount)
{
    if (m_capacity == 0) setCapacity(65536);  // until the renderer's first frame sets surface.max_bricks
    for (size_t n = 0; n < removedCount; ++n)
    {
        const Key k{ removedKeys[3 * n], removedKeys[3 * n + 1], removedKeys[3 * n + 2] };
        auto it = m_slotOf.find(k);
        if (it == m_slotOf.end()) fail("surface: removed brick (%d, %d, %d) is unknown", k.x, k.y, k.z);
        erase(k);
        m_free.push_back(it->second);
        m_slotOf.erase(it);
    }
    for (size_t n = 0; n < changedCount; ++n)
    {
        const BrickInput& b = changed[n];
        const Key k{ b.key[0], b.key[1], b.key[2] };
        uint32_t slot;
        auto it = m_slotOf.find(k);
        if (it != m_slotOf.end()) slot = it->second;
        else
        {
            if (!m_free.empty())
            {
                slot = m_free.back();
                m_free.pop_back();
            }
            else if (m_next < m_capacity) slot = m_next++;
            else fail("surface: more than surface.max_bricks = %u bricks", m_capacity);
            m_slotOf[k] = slot;
            insert(k, slot);
        }
        encode(b, m_records.data() + (size_t)slot * kBrickBytes);
        if (!m_slotDirty[slot])
        {
            m_slotDirty[slot] = 1;
            m_dirtySlots.push_back(slot);
        }
    }
}

std::vector<uint32_t> SurfaceField::takeDirtySlots()
{
    for (uint32_t s : m_dirtySlots) m_slotDirty[s] = 0;
    return std::move(m_dirtySlots);
}
std::vector<uint32_t> SurfaceField::takeDirtyEntries()
{
    for (uint32_t e : m_dirtyEntries) m_entryDirty[e] = 0;
    return std::move(m_dirtyEntries);
}
bool SurfaceField::takeWhole()
{
    const bool w = m_whole;
    m_whole = false;
    return w;
}

std::vector<uint32_t> SurfaceField::liveSlots() const
{
    std::vector<uint32_t> out;
    out.reserve(m_slotOf.size());
    for (const auto& [k, s] : m_slotOf) out.push_back(s);
    std::sort(out.begin(), out.end());
    return out;
}

SurfaceField& surfaceField(TrackState& state) { return state.get<SurfaceField>("surface.field"); }

namespace
{
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
          "surface buffer");
    r->SetName(name);
    return r;
}

class SurfacePass
{
public:
    explicit SurfacePass(Device& device) : m_device(device) {}
    ~SurfacePass()
    {
        m_device.waitIdle();
        for (Slot& s : m_slots)
            if (s.upload && s.mapped) s.upload->Unmap(0, nullptr);
    }
    void record(FramePassContext& fc, SurfaceField& field)
    {
        field.setCapacity((uint32_t)fc.quality.integer("surface.max_bricks"));
        if (field.bricks() == 0 && !m_pool) return;  // nothing yet: no buffers, no passes
        const uint32_t capacity = field.capacity(), tableSize = field.tableSize();
        if (!m_pool || m_capacity != capacity || m_tableSize != tableSize)
        {
            if (m_pool) m_device.deferRelease(m_pool);
            if (m_table) m_device.deferRelease(m_table);
            m_pool = makeBuffer(m_device, (uint64_t)capacity * kBrickBytes, D3D12_HEAP_TYPE_DEFAULT, L"surface pool");
            m_table = makeBuffer(m_device, (uint64_t)tableSize * 16, D3D12_HEAP_TYPE_DEFAULT, L"surface table");
            m_constants = makeBuffer(m_device, 256, D3D12_HEAP_TYPE_DEFAULT, L"surface constants");
            m_capacity = capacity;
            m_tableSize = tableSize;
            m_whole = true;
        }
        if (m_slots.size() != std::max(fc.framesInFlight, 1u))
        {
            m_device.waitIdle();
            for (Slot& s : m_slots)
                if (s.upload && s.mapped) s.upload->Unmap(0, nullptr);
            m_slots.assign(std::max(fc.framesInFlight, 1u), Slot{});
        }
        const bool whole = field.takeWhole() || m_whole;
        m_whole = false;
        std::vector<uint32_t> slots = field.takeDirtySlots(), entries = field.takeDirtyEntries();
        if (whole)
        {
            slots = field.liveSlots();  // empty slots are never read (the table names only live ones)
            entries.clear();
            for (uint32_t e = 0; e < tableSize; ++e) entries.push_back(e);
        }
        // staging: constants (48 B, padded 256), brick records (117 words), table records (5 words)
        const uint64_t brickBytes = (uint64_t)slots.size() * 117 * 4, entryBytes = (uint64_t)entries.size() * 20;
        const uint64_t bytes = 256 + brickBytes + entryBytes;
        Slot& slot = m_slots[fc.frame.frameIndex % m_slots.size()];
        if (!slot.upload || slot.bytes < bytes)
        {
            if (slot.upload)
            {
                slot.upload->Unmap(0, nullptr);
                m_device.deferRelease(slot.upload);
            }
            slot.bytes = std::max<uint64_t>(bytes, 64 * 1024);
            slot.upload = makeBuffer(m_device, slot.bytes, D3D12_HEAP_TYPE_UPLOAD, L"surface upload");
            D3D12_RANGE none{ 0, 0 };
            check(slot.upload->Map(0, &none, reinterpret_cast<void**>(&slot.mapped)), "map surface upload");
        }
        uint8_t* p = slot.mapped;
        uint32_t c[12] = {};
        c[0] = tableSize - 1;
        c[1] = field.maxProbe();
        c[2] = (uint32_t)field.bricks();
        const float now = (float)field.now();
        std::memcpy(&c[4], &now, 4);
        for (uint32_t k = 0; k < kChannels; ++k)
        {
            const double h = field.halfLives()[k];
            const float inv = h > 0 ? (float)(1.0 / h) : 0.0f;
            std::memcpy(&c[5 + k], &inv, 4);
        }
        std::memset(p, 0, 256);
        std::memcpy(p, c, sizeof c);
        uint8_t* q = p + 256;
        for (uint32_t s : slots)
        {
            std::memcpy(q, &s, 4);
            std::memcpy(q + 4, field.records().data() + (size_t)s * kBrickBytes, kBrickBytes);
            q += 117 * 4;
        }
        for (uint32_t e : entries)
        {
            std::memcpy(q, &e, 4);
            std::memcpy(q + 4, field.table()[e].data(), 16);
            q += 20;
        }

        RenderGraph& g = fc.graph;
        const BufferRef pool = g.importBuffer(m_pool.Get(), BufferDesc{ "surface.pool", (uint64_t)capacity * kBrickBytes, 0 });
        const BufferRef table = g.importBuffer(m_table.Get(), BufferDesc{ "surface.table", (uint64_t)tableSize * 16, 0 });
        const BufferRef constants = g.importBuffer(m_constants.Get(), BufferDesc{ "surface.constants", 256, 0 });
        ID3D12Resource* upload = slot.upload.Get();
        const uint32_t nb = (uint32_t)slots.size(), ne = (uint32_t)entries.size();
        const BufferRef staged = (nb || ne) ? g.createBuffer(BufferDesc{ "surface.staged", std::max<uint64_t>(brickBytes + entryBytes, 16), 0 }) : BufferRef{};
        g.addPass("surface.upload", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(constants, Use::CopyDst);
                      if (staged.valid()) b.use(staged, Use::CopyDst);
                  },
                  [=](PassContext& ctx) {
                      ctx.cmd->CopyBufferRegion(ctx.resource(constants), 0, upload, 0, 256);
                      if (staged.valid()) ctx.cmd->CopyBufferRegion(ctx.resource(staged), 0, upload, 256, brickBytes + entryBytes);
                  });
        auto scatter = [&](const char* name, const char* kernel, BufferRef target, uint32_t count, uint32_t offset, uint32_t perGroup) {
            if (!count) return;
            ID3D12PipelineState* pso = fc.shaders.compute(kernel);
            g.addPass(name, QueueType::Graphics,
                      [=](PassBuilder& b) {
                          b.use(staged, Use::SrvCompute);
                          b.use(target, Use::UavCompute);
                      },
                      [=](PassContext& ctx) {
                          ctx.cmd->SetPipelineState(pso);
                          for (uint32_t first = 0; first < count; first += 65535u * perGroup)
                          {
                              const uint32_t k[8] = { ctx.srv(staged), ctx.uav(target), count, first, offset, 0, 0, 0 };
                              ctx.computeConstants(k, 8);
                              ctx.cmd->Dispatch(std::min((count - first + perGroup - 1) / perGroup, 65535u), 1, 1);
                          }
                      });
        };
        // brick records first (117 words each), then the table records (5 words each) in the one staged buffer
        if (brickBytes + entryBytes > UINT32_MAX) fail("surface: %llu bytes to upload in one frame", (unsigned long long)(brickBytes + entryBytes));
        scatter("surface.bricks", "Passes/Decal/SurfaceUpload.MODE0", pool, nb, 0, 1);
        scatter("surface.entries", "Passes/Decal/SurfaceUpload.MODE1", table, ne, (uint32_t)brickBytes, 64);
        fc.resources.surfaceConstants = constants;
        fc.resources.surfaceTable = table;
        fc.resources.surfacePool = pool;
    }

private:
    struct Slot
    {
        ComPtr<ID3D12Resource> upload;
        uint8_t* mapped = nullptr;
        uint64_t bytes = 0;
    };
    Device& m_device;
    ComPtr<ID3D12Resource> m_pool, m_table, m_constants;
    uint32_t m_capacity = 0, m_tableSize = 0;
    bool m_whole = true;
    std::vector<Slot> m_slots;
};
} // namespace
} // namespace unx::surface

namespace unx::render::tracks
{
void surfaceState(FramePassContext& fc)
{
    if (!fc.trackState) return;
    auto& p = fc.trackState->get<std::unique_ptr<surface::SurfacePass>>("surface.pass");
    if (!p) p = std::make_unique<surface::SurfacePass>(fc.device);
    p->record(fc, surface::surfaceField(*fc.trackState));
}
} // namespace unx::render::tracks
