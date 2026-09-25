// GPU particle module (WORLD_VFX_DESIGN_KO.md 3.3, stream NativeVfxStream.h). See include/unx/fx/Particles.h.
#include "unx/fx/Particles.h"

#include "unx/core/Log.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

namespace unx::fx
{
using namespace unx::render;

namespace
{
constexpr uint32_t kScanBlock = 1024;
constexpr uint32_t kReportBytes = 64;
// FxTick constant buffer: TickConstants, then kCbFields context fields and kCbWorldFields world fields (Particles.hlsli)
constexpr uint32_t kCbFields = 64, kCbWorldFields = 16;
constexpr uint64_t kFieldRowsOffset = sizeof(TickConstants), kWorldRowsOffset = kFieldRowsOffset + kCbFields * 32,
                   kConstBytes = (kWorldRowsOffset + kCbWorldFields * 64 + 255) / 256 * 256;
static_assert(sizeof(NV_StreamField) == 32 && sizeof(NV_StreamWorldField) == 64, "FxTick field rows");
static_assert(sizeof(TickConstants) % 16 == 0, "FxTick rows start on a 16-byte boundary");
constexpr uint32_t kOverflowRecords = 1024, kOverflowRecordBytes = 432;  // IMPACT_OVERFLOW diagnostic records (Particles.hlsli)
constexpr float kKeyFar = 1.0e6f;          // metres: log range of the sort key (keyNear .. 1000 km)

uint32_t groups(uint64_t n, uint32_t size) { return (uint32_t)((n + size - 1) / size); }
uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "FX particle buffer");
    r->SetName(name);
    return r;
}

// A persistent default buffer, imported into each frame's graph.
struct Buf
{
    const char* name = "";
    uint32_t stride = 0;
    uint64_t bytes = 0;
    ComPtr<ID3D12Resource> resource;
    BufferRef ref;
    const RenderGraph* importedGraph = nullptr;  // imported once per frame: every tick of the frame uses the same ref
    uint64_t importedFrame = UINT64_MAX;
    ID3D12Resource* importedResource = nullptr;

    void ensure(Device& device, uint64_t need)
    {
        need = std::max<uint64_t>(need, 256);
        if (resource && bytes >= need) return;
        if (resource) device.deferRelease(resource);
        uint64_t size = 256;
        while (size < need) size <<= 1;
        bytes = size;
        if (stride) bytes = align(bytes, stride);
        std::wstring w(name, name + std::strlen(name));
        resource = makeBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, (L"FX " + w).c_str());
    }
    BufferRef import(RenderGraph& graph, uint64_t frame)
    {
        if (importedGraph == &graph && importedFrame == frame && importedResource == resource.Get()) return ref;
        ref = graph.importBuffer(resource.Get(), BufferDesc{ name, bytes, stride });
        importedGraph = &graph;
        importedFrame = frame;
        importedResource = resource.Get();
        return ref;
    }
};

const NV_StreamHeader& headerOf(const std::vector<uint8_t>& packet) { return *reinterpret_cast<const NV_StreamHeader*>(packet.data()); }

// Everything the CPU sends is checked here (counts, sections, indices into the packet and into the persistent
// program and surface tables), so the kernels only meet indices the packet declares.
void validate(const uint8_t* packet, uint64_t bytes, uint32_t chainDepthMax, uint32_t programCount, uint32_t surfaceBodyMax)
{
    if (!packet || bytes < sizeof(NV_StreamHeader)) fail("FX particles: stream packet of %llu bytes has no header", (unsigned long long)bytes);
    const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet);
    if (h.magic != NV_STREAM_MAGIC || h.version != NV_STREAM_VERSION) fail("FX particles: stream packet magic/version %llx/%u", (unsigned long long)h.magic, h.version);
    if (h.bytes != bytes) fail("FX particles: stream packet says %llu bytes, got %llu", (unsigned long long)h.bytes, (unsigned long long)bytes);
    auto section = [&](uint64_t offset, uint64_t count, uint64_t stride, const char* what) {
        if (count == 0) return;
        if (offset % 16 != 0 || offset < sizeof(NV_StreamHeader) || offset + count * stride > bytes)
            fail("FX particles: stream section %s (offset %llu, %llu x %llu B) outside the %llu-byte packet", what, (unsigned long long)offset,
                 (unsigned long long)count, (unsigned long long)stride, (unsigned long long)bytes);
    };
    section(h.programs, h.program_count, sizeof(NV_StreamProgram), "programs");
    section(h.curve_keys, h.curve_key_count, sizeof(NV_StreamCurveKey), "curve keys");
    section(h.emitters, h.emitter_count, sizeof(NV_StreamEmitter), "emitters");
    section(h.emitter_patches, h.emitter_patch_count, sizeof(NV_StreamEmitterPatch), "emitter patches");
    if (h.flags & NV_STREAM_EMITTER_DELTA)
    {
        section(h.emitter_rows, h.emitter_count, 4, "emitter rows");
        if (h.flags & NV_STREAM_RESET) fail("FX particles: a RESET packet sends the whole emitter table");
    }
    else if (h.emitter_count != h.emitter_table || h.emitter_patch_count)
        fail("FX particles: whole emitter table of %u blocks (%u patches) for %u rows", h.emitter_count, h.emitter_patch_count, h.emitter_table);
    section(h.spawns, h.spawn_count, sizeof(NV_StreamSpawn), "spawns");
    section(h.explicit_births, h.explicit_count, sizeof(NV_StreamExplicitBirth), "explicit births");
    section(h.fields, h.field_count, sizeof(NV_StreamField), "fields");
    section(h.world_fields, h.world_field_count, sizeof(NV_StreamWorldField), "world fields");
    section(h.surfaces, h.surface_count, sizeof(NV_StreamSurface), "surfaces");
    section(h.restore, h.restore_count, sizeof(NV_StreamParticle), "restore");
    section(h.bodies, h.body_count, sizeof(NV_StreamBody), "bodies");
    section(h.dynamic_surfaces, h.dynamic_surface_count, sizeof(NV_StreamSurface), "dynamic surfaces");
    if (h.alive_after > h.slot_capacity) fail("FX particles: alive_after %u > capacity %u", h.alive_after, h.slot_capacity);
    if (h.restore_count > h.slot_capacity) fail("FX particles: %u restore records > capacity %u", h.restore_count, h.slot_capacity);
    if (h.depth[0] != 0 || h.depth[NV_STREAM_MAX_DEPTH + 1] != h.spawn_count) fail("FX particles: spawn depth ranges do not cover the records");
    for (uint32_t d = 0; d <= NV_STREAM_MAX_DEPTH; ++d)
    {
        if (h.depth[d] > h.depth[d + 1]) fail("FX particles: spawn depth ranges not ascending");
        if (d > chainDepthMax && h.depth[d + 1] > h.depth[d]) fail("FX particles: depth %u records beyond fx.particles.chain_depth_max %u", d, chainDepthMax);
    }
    if (h.dt < 0 || !std::isfinite(h.dt)) fail("FX particles: dt %g", h.dt);
    const auto* emitters = reinterpret_cast<const NV_StreamEmitter*>(packet + h.emitters);
    const auto* programs = reinterpret_cast<const NV_StreamProgram*>(packet + h.programs);
    const auto* spawns = reinterpret_cast<const NV_StreamSpawn*>(packet + h.spawns);
    for (uint32_t k = 0; k < h.spawn_count; ++k)
    {
        if (spawns[k].emitter >= h.emitter_table) fail("FX particles: spawn record %u names emitter row %u of %u", k, spawns[k].emitter, h.emitter_table);
        if (spawns[k].expired > spawns[k].count) fail("FX particles: spawn record %u expired %u > count %u", k, spawns[k].expired, spawns[k].count);
    }
    const uint32_t programTable = (h.flags & NV_STREAM_PROGRAMS) ? h.program_count : programCount;
    const auto* rows = reinterpret_cast<const uint32_t*>(packet + h.emitter_rows);
    const bool delta = (h.flags & NV_STREAM_EMITTER_DELTA) != 0;
    for (uint32_t k = 0; k < h.emitter_count; ++k)
    {
        const uint32_t e = delta ? rows[k] : k;
        if (e >= h.emitter_table || (delta && k && rows[k - 1] >= e)) fail("FX particles: emitter block %u names row %u (table %u, ascending rows)", k, e, h.emitter_table);
        const NV_StreamEmitter& x = emitters[k];
        if (!(x.flags & NV_STREAM_EMITTER_ACTIVE)) continue;
        if (x.program >= programTable) fail("FX particles: emitter row %u names program %u of %u", e, x.program, programTable);
        if (x.parent_event != NV_STREAM_NONE && (x.parent_event >= h.event_slots || x.parent_row >= h.emitter_table))
            fail("FX particles: emitter row %u: parent event %u / row %u outside %u events, %u rows", e, x.parent_event, x.parent_row, h.event_slots, h.emitter_table);
    }
    // patches: ascending rows inside the table, none also sent as a block, no transport/source (those rows send blocks)
    const auto* patches = reinterpret_cast<const NV_StreamEmitterPatch*>(packet + h.emitter_patches);
    for (uint32_t k = 0, b = 0; k < h.emitter_patch_count; ++k)
    {
        const NV_StreamEmitterPatch& x = patches[k];
        if (x.row >= h.emitter_table || (k && patches[k - 1].row >= x.row) || (x.flags & (NV_STREAM_EMITTER_TRANSPORT | NV_STREAM_EMITTER_SOURCE)))
            fail("FX particles: emitter patch %u (row %u, flags 0x%x) out of order, outside %u rows or with transport/source", k, x.row, x.flags, h.emitter_table);
        while (b < h.emitter_count && rows[b] < x.row) ++b;
        if (b < h.emitter_count && rows[b] == x.row) fail("FX particles: row %u has both a block and a patch", x.row);
        if (x.parent_event != NV_STREAM_NONE && (x.parent_event >= h.event_slots || x.parent_row >= h.emitter_table))
            fail("FX particles: emitter patch of row %u: parent event %u / row %u outside %u events, %u rows", x.row, x.parent_event, x.parent_row, h.event_slots, h.emitter_table);
    }
    for (uint32_t k = 0; k < h.spawn_count; ++k)
    {
        const NV_StreamSpawn& r = spawns[k];
        if (r.birth_event != NV_STREAM_NONE && (uint64_t)r.birth_event + r.count > h.event_slots) fail("FX particles: spawn record %u birth events outside %u slots", k, h.event_slots);
        if (r.death_event != NV_STREAM_NONE && (uint64_t)r.death_event + r.expired > h.event_slots) fail("FX particles: spawn record %u death events outside %u slots", k, h.event_slots);
    }
    const auto* explicits = reinterpret_cast<const NV_StreamExplicitBirth*>(packet + h.explicit_births);
    for (uint32_t k = 0; k < h.explicit_count; ++k)
    {
        const NV_StreamExplicitBirth& x = explicits[k];
        if (x.emitter >= h.emitter_table) fail("FX particles: explicit birth %u names row %u of %u", k, x.emitter, h.emitter_table);
        if ((x.birth_event != NV_STREAM_NONE && x.birth_event >= h.event_slots) || (x.death_event != NV_STREAM_NONE && x.death_event >= h.event_slots))
            fail("FX particles: explicit birth %u event slots outside %u", k, h.event_slots);
    }
    uint32_t bodyMax = surfaceBodyMax;  // largest body index + 1 of the surface table in use
    if (h.flags & (NV_STREAM_SURFACES | NV_STREAM_RESET))
    {
        bodyMax = 0;
        const auto* surfaces = reinterpret_cast<const NV_StreamSurface*>(packet + h.surfaces);
        for (uint32_t k = 0; k < h.surface_count; ++k)
            if (surfaces[k].body != NV_STREAM_NONE) bodyMax = std::max(bodyMax, surfaces[k].body + 1);
    }
    const auto* dynamicRows = reinterpret_cast<const NV_StreamSurface*>(packet + h.dynamic_surfaces);
    for (uint32_t k = 0; k < h.dynamic_surface_count; ++k)
        if (dynamicRows[k].body != NV_STREAM_NONE) bodyMax = std::max(bodyMax, dynamicRows[k].body + 1);
    if (h.dt > 0 && bodyMax > h.body_count) fail("FX particles: surfaces reference body %u, the packet has %u bodies", bodyMax - 1, h.body_count);
    (void)programs;
}
} // namespace

struct ParticleSystem::Impl
{
    // state (capacity sized)
    // state pair by tick parity: [cur] = this tick's output, [cur ^ 1] = its input (the last tick's output + births)
    Buf posAge[2] = { { "fx.posAge0", 16 }, { "fx.posAge1", 16 } }, velocity[2] = { { "fx.velocity0", 16 }, { "fx.velocity1", 16 } };
    Buf meta{ "fx.meta", 8 }, alive{ "fx.alive", 4 };
    Buf aliveList{ "fx.aliveList", 4 }, deadList{ "fx.deadList", 4 }, dyingList{ "fx.dyingList", 4 };
    Buf blockSums{ "fx.blockSums", 8 }, spawnedSlots{ "fx.spawnedSlots", 4 };
    Buf emitterTable{ "fx.emitterTable", sizeof(NV_StreamEmitter) }, emitterStamp{ "fx.emitterStamp", 4 };  // persistent (delta updates)
    Buf emitterUpdates{ "fx.emitterUpdates", sizeof(NV_StreamEmitter) }, emitterUpdateRows{ "fx.emitterUpdateRows", 4 };
    Buf emitterPatches{ "fx.emitterPatches", sizeof(NV_StreamEmitterPatch) };
    std::vector<NV_StreamEmitter> table;  // CPU mirror of the persistent table (ribbon and volume ranges)
    uint32_t tableRows = 0, serial = 0;
    float separationMax = 0;  // largest program separation (collision grid motion bound)
    std::vector<uint32_t> programGrid;  // medium_grid per program (volume ranges)
    std::vector<uint32_t> tickRows;     // mirror rows whose last block carried a per-tick field (cleared next tick)
    std::vector<uint32_t> outputRows;   // active ribbon/volume rows of the mirror (unordered; ranges are sorted)
    std::vector<uint8_t> outputFlag;
    Buf dynamic[2] = { { "fx.emitterDynamic0", 32 }, { "fx.emitterDynamic1", 32 } };
    Buf counters{ "fx.counters", 4 }, report{ "fx.report", 4 };
    // inputs
    Buf programs{ "fx.programs", sizeof(NV_StreamProgram) }, curveKeys{ "fx.curveKeys", 16 };
    Buf spawns{ "fx.spawns", sizeof(NV_StreamSpawn) }, explicitBirths{ "fx.explicitBirths", sizeof(NV_StreamExplicitBirth) };
    Buf fields{ "fx.fields", sizeof(NV_StreamField) }, worldFields{ "fx.worldFields", sizeof(NV_StreamWorldField) };
    Buf surfaces{ "fx.surfaces", sizeof(NV_StreamSurface) }, restore{ "fx.restore", sizeof(NV_StreamParticle) }, slotBase{ "fx.slotBase", 4 };
    Buf dynamicSurfaces{ "fx.dynamicSurfaces", sizeof(NV_StreamSurface) };
    Buf bodies{ "fx.bodies", sizeof(NV_StreamBody) }, tickSurfaces{ "fx.tickSurfaces", sizeof(NV_StreamSurface) };
    Buf ribbonPoints{ "fx.ribbonPoints", 32 }, ribbonLinks{ "fx.ribbonLinks", 4 }, ribbonVertices{ "fx.ribbonVertices", 32 };
    Buf ribbonRanges{ "fx.ribbonRanges", 16 }, ribbonRunStart{ "fx.ribbonRunStart", 4 }, ribbonTangents{ "fx.ribbonTangents", 16 };
    std::vector<uint32_t> programOutput;  // output kind per program (the last NV_STREAM_PROGRAMS table)
    Buf surfaceBoxes{ "fx.surfaceBoxes", 16 };
    Buf colliders{ "fx.colliders", 48 };
    Buf trace{ "fx.trace", 528 };  // TraceRecord of the traced particle (diagnostic)  // colliding slots after their motion (FxIntegrate -> FxCollide)
    Buf overflowRecords{ "fx.overflowRecords", kOverflowRecordBytes };  // IMPACT_OVERFLOW inputs (diagnostic, readState "overflow")
    // local volume particles: record16 + side8 per live volume particle (Particles.hlsli; render rules request 3b)
    Buf volumeRecords{ "fx.volumeRecords", 16 }, volumeSide{ "fx.volumeSide", 8 }, volumeRanges{ "fx.volumeRanges", 32 }, gridBlocks{ "fx.gridBlocks", 4 };
    uint32_t lastCollisions = 0;  // collision events of the last tick the CPU read (readback prefix size)
    Buf gridCount{ "fx.gridCount", 4 }, gridStart{ "fx.gridStart", 4 }, gridFill{ "fx.gridFill", 4 }, gridEntries{ "fx.gridEntries", 4 }, gridLarge{ "fx.gridLarge", 4 };
    bool programsValid = false, started = false;
    std::vector<ComPtr<ID3D12Resource>> graveyard;  // buffers a repack of the last recorded frame still read
    uint64_t graveyardFrame = UINT64_MAX;
    uint32_t parity = 0;
    uint32_t surfaceCount = 0;                           // persistent surface table (NV_STREAM_SURFACES)
    uint32_t submittedPrograms = 0, submittedBodyMax = 0;  // tables as of the last submitted packet (validation)

    struct Slot
    {
        ComPtr<ID3D12Resource> upload;
        uint64_t uploadBytes = 0;
        uint8_t* mapped = nullptr;
        Buf events{ "fx.events", sizeof(NV_StreamEvent) };
        ComPtr<ID3D12Resource> readback;
        uint64_t readbackBytes = 0;
        // the tick it holds
        bool recorded = false;
        uint64_t stream = 0, generation = 0, tick = 0;
        uint32_t eventSlots = 0, collisionCopy = 0, collisionCapacity = 0;
        uint64_t fenceBase = 0, fence = 0;
    };
    std::vector<Slot> slots;
    uint32_t nextSlot = 0;
    int latestSlot = -1;

    // The frame that recorded a tick signals the graphics queue after record() returns, so its fence is the first value
    // signaled after the recording: resolved from the device queue (which outlives any render graph), conservatively
    // (a later signal also covers it). A tick whose frame never executed cannot be waited for: that is a caller error.
    Queue* queue = nullptr;
    // Bound of a readback wait: 20 s on a GPU. A software adapter (WARP) compiles each kernel to CPU code on its first
    // dispatch, which for the collision kernels takes tens of seconds on a loaded machine, so it gets 10 min.
    DWORD fenceWaitMs = 20000;
    void resolveFence(Slot& s)
    {
        if (!s.recorded || s.fence != 0 || !queue) return;
        const uint64_t f = queue->lastSignaled();
        if (f > s.fenceBase) s.fence = f;
    }
    void waitSlot(Device& device, Slot& s)
    {
        if (!s.recorded) return;
        resolveFence(s);
        if (s.fence == 0) fail("FX particles: tick %llu was recorded but its frame was never executed", (unsigned long long)s.tick);
        Queue& q = device.queue(QueueType::Graphics);
        if (q.completed() >= s.fence) return;
        // event wait with a bound (a sleep loop would round up to the OS timer tick, ~15 ms)
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(q.fence()->SetEventOnCompletion(s.fence, ev), "FX particles: fence event");
        const DWORD r = WaitForSingleObject(ev, fenceWaitMs);
        CloseHandle(ev);
        if (r != WAIT_OBJECT_0)
            fail("FX particles: tick %llu: the GPU did not reach fence %llu within %lu s (completed %llu)", (unsigned long long)s.tick,
                 (unsigned long long)s.fence, (unsigned long)(fenceWaitMs / 1000), (unsigned long long)q.completed());
    }
};

ParticleSystem::ParticleSystem(Device& device, const QualityConfig& quality) : m_impl(std::make_unique<Impl>()), m_device(device)
{
    DXGI_ADAPTER_DESC3 adapter{};
    if (device.adapter() && SUCCEEDED(device.adapter()->GetDesc3(&adapter)) && (adapter.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE))
        m_impl->fenceWaitMs = 600000;
    m_chainDepthMax = (uint32_t)quality.integer("fx.particles.chain_depth_max");
    m_readbackSlots = (uint32_t)quality.integer("fx.particles.readback_slots");
    m_collisionReadback = (uint32_t)quality.integer("fx.particles.collision_readback");
    m_gridCell = (float)quality.number("fx.particles.collision_cell_m");
    m_experimentDisable = (uint32_t)quality.integer("fx.particles.experiment_disable");
    if (!(m_gridCell > 0)) fail("fx.particles.collision_cell_m must be > 0");
    if (m_chainDepthMax > NV_STREAM_MAX_DEPTH) fail("fx.particles.chain_depth_max must be <= %u", NV_STREAM_MAX_DEPTH);
    if (m_readbackSlots < 2) fail("fx.particles.readback_slots must be >= 2");
    m_impl->slots.resize(m_readbackSlots);
}

ParticleSystem::~ParticleSystem()
{
    m_device.waitIdle();
    for (auto& s : m_impl->slots)
        if (s.upload && s.mapped) s.upload->Unmap(0, nullptr);
}

void ParticleSystem::submit(const uint8_t* packet, uint64_t bytes)
{
    Impl& m = *m_impl;
    validate(packet, bytes, m_chainDepthMax, m.submittedPrograms, m.submittedBodyMax);
    const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet);
    if (h.flags & NV_STREAM_PROGRAMS) m.submittedPrograms = h.program_count;
    if (h.flags & (NV_STREAM_SURFACES | NV_STREAM_RESET))
    {
        m.submittedBodyMax = 0;
        const auto* surfaces = reinterpret_cast<const NV_StreamSurface*>(packet + h.surfaces);
        for (uint32_t k = 0; k < h.surface_count; ++k)
            if (surfaces[k].body != NV_STREAM_NONE) m.submittedBodyMax = std::max(m.submittedBodyMax, surfaces[k].body + 1);
    }
    m_pending.emplace_back(packet, packet + bytes);
}

void ParticleSystem::record(FramePassContext& fc)
{
    Impl& m = *m_impl;
    Device& device = fc.device;
    while (!m_pending.empty())
    {
        std::vector<uint8_t> packet = std::move(m_pending.front());
        m_pending.pop_front();
        const NV_StreamHeader& h = headerOf(packet);
        const uint8_t* base = packet.data();

        // Fences of the ticks recorded in earlier frames: the first signal after their recording is their frame's.
        m.queue = &device.queue(QueueType::Graphics);
        for (auto& x : m.slots) m.resolveFence(x);

        // Buffers the previous frame's repack read are released now (after that frame's fence).
        if (m.graveyardFrame != fc.frame.frameIndex)
        {
            for (auto& r : m.graveyard) device.deferRelease(r);
            m.graveyard.clear();
        }

        // Capacity: the stream sizes it for each tick's need (NativeVfxStream.h). A change without RESET moves the live
        // slots into new buffers (FxRepack) and keeps every particle; with RESET the state starts over anyway.
        const uint32_t capacity = h.slot_capacity;
        const bool reset = (h.flags & NV_STREAM_RESET) != 0 || !m.started;  // (a capacity of 0 is valid: no live slot)
        m.started = true;
        const bool repack = !reset && capacity != m_capacity;
        struct Old { Buf list, posAge, velocity, meta; } old;
        if (capacity != m_capacity)
        {
            if (repack)
            {
                const uint32_t prev = m.parity ^ 1u;  // output of the last recorded tick = this tick's input
                old = { m.aliveList, m.posAge[prev], m.velocity[prev], m.meta };
                for (Buf* b : { &old.list, &old.posAge, &old.velocity, &old.meta })
                {
                    m.graveyard.push_back(b->resource);
                    b->importedGraph = nullptr;  // import this frame under its own name
                }
            }
            // every capacity-sized buffer is allocated for the new capacity (a shrink frees memory too)
            for (Buf* b : { &m.posAge[0], &m.posAge[1], &m.velocity[0], &m.velocity[1], &m.meta, &m.alive, &m.aliveList, &m.deadList, &m.dyingList,
                            &m.spawnedSlots })
            {
                if (b->resource && !repack) device.deferRelease(b->resource);
                else if (b->resource && std::find(m.graveyard.begin(), m.graveyard.end(), b->resource) == m.graveyard.end()) m.graveyard.push_back(b->resource);
                b->resource.Reset();
                b->bytes = 0;
            }
            m_capacity = capacity;
            m.graveyardFrame = fc.frame.frameIndex;
        }
        const uint32_t scanBlocks = groups(capacity, kScanBlock);
        for (Buf* b : { &m.posAge[0], &m.posAge[1], &m.velocity[0], &m.velocity[1], &m.meta, &m.alive, &m.aliveList, &m.deadList, &m.dyingList,
                        &m.spawnedSlots })
            b->ensure(device, (uint64_t)capacity * b->stride);
        m.gridBlocks.ensure(device, 1024 * 4);
        m.blockSums.ensure(device, (uint64_t)scanBlocks * 8);
        m.counters.ensure(device, kCounterWords * 4);
        m.report.ensure(device, kReportBytes);

        // Programs persist until a packet replaces them.
        if (h.flags & NV_STREAM_PROGRAMS)
        {
            const auto* programTable = reinterpret_cast<const NV_StreamProgram*>(base + h.programs);
            m.programOutput.resize(h.program_count);
            m.programGrid.resize(h.program_count);
            m.separationMax = 0;
            for (uint32_t k = 0; k < h.program_count; ++k)
            {
                m.programOutput[k] = programTable[k].output;
                m.programGrid[k] = programTable[k].medium_grid;
                m.separationMax = std::max(m.separationMax, std::abs(programTable[k].separation));
            }
            m.programs.ensure(device, (uint64_t)h.program_count * sizeof(NV_StreamProgram));
            m.curveKeys.ensure(device, (uint64_t)h.curve_key_count * 16);
            m.programsValid = true;
        }
        if (!m.programsValid) fail("FX particles: the first packet of a stream must carry the program table (NV_STREAM_PROGRAMS)");

        const uint32_t cur = m.parity;
        m.parity ^= 1u;
        // Persistent emitter table: a whole table replaces it, a delta updates the rows it lists (the table only grows).
        // A grown table keeps its rows: the old buffer is copied into the new one at the start of the tick.
        const bool delta = (h.flags & NV_STREAM_EMITTER_DELTA) != 0;
        const uint32_t tableRows = h.emitter_table;
        Buf grownFrom{ "fx.emitterTableOld", sizeof(NV_StreamEmitter) };  // a delta keeps the rows of a grown table (copied below)
        if (m.emitterTable.bytes < (uint64_t)tableRows * sizeof(NV_StreamEmitter))
        {
            if (m.emitterTable.resource && delta)
            {
                grownFrom.resource = m.emitterTable.resource;  // released after this frame (deferRelease once the copy is recorded)
                grownFrom.bytes = m.emitterTable.bytes;
                m.emitterTable.resource.Reset();
                m.emitterTable.bytes = 0;
            }
            m.emitterTable.ensure(device, (uint64_t)tableRows * sizeof(NV_StreamEmitter));
        }
        // stamps start at 0 in a new buffer (committed resources are zeroed); the serial starts at 1
        m.emitterStamp.ensure(device, (m.emitterTable.bytes / sizeof(NV_StreamEmitter)) * 4);
        const auto* emitterBlocks = reinterpret_cast<const NV_StreamEmitter*>(base + h.emitters);
        const auto* blockRows = reinterpret_cast<const uint32_t*>(base + h.emitter_rows);
        std::vector<uint32_t> updateRows(std::max<uint32_t>(h.emitter_count, 1)), patchedRows;
        // The mirror costs O(blocks + rows with per-tick fields), not O(rows): only the rows whose last block carried a
        // per-tick field are cleared, and the ribbon/volume rows are kept as a list (a row joins when a block makes it an
        // output row; inactive or changed rows leave when the ranges are built).
        if (delta)
        {
            m.table.resize(tableRows);
            m.outputFlag.resize(tableRows, 0);
            for (uint32_t row : m.tickRows)
            {
                NV_StreamEmitter& e = m.table[row];
                e.rebase[0] = e.rebase[1] = e.rebase[2] = 0;
                e.flags &= ~uint32_t(NV_STREAM_EMITTER_TRANSPORT | NV_STREAM_EMITTER_SOURCE | NV_STREAM_EMITTER_KILLED);
                e.parent_event = e.parent_row = NV_STREAM_NONE;
            }
            m.tickRows.clear();
            for (uint32_t k = 0; k < h.emitter_count; ++k) { m.table[blockRows[k]] = emitterBlocks[k]; updateRows[k] = blockRows[k]; }
            const auto* patches = reinterpret_cast<const NV_StreamEmitterPatch*>(base + h.emitter_patches);
            for (uint32_t k = 0; k < h.emitter_patch_count; ++k)
            {
                const NV_StreamEmitterPatch& p = patches[k];
                NV_StreamEmitter& x = m.table[p.row];
                x.flags = p.flags;
                x.next_birth = p.next_birth; x.death_birth = p.death_birth; x.dying_birth = p.dying_birth; x.death_event = p.death_event;
                x.output_base = p.output_base; x.parent_event = p.parent_event; x.parent_row = p.parent_row;
                std::memcpy(x.rebase, p.rebase, sizeof x.rebase);
                patchedRows.push_back(p.row);
            }
        }
        else
        {
            m.table.assign(emitterBlocks, emitterBlocks + h.emitter_count);
            m.outputFlag.assign(tableRows, 0);
            m.outputRows.clear();
            m.tickRows.clear();
            for (uint32_t k = 0; k < h.emitter_count; ++k) updateRows[k] = k;
        }
        for (uint32_t k = 0; k < h.emitter_count + (uint32_t)patchedRows.size(); ++k)
        {
            const uint32_t row = k < h.emitter_count ? updateRows[k] : patchedRows[k - h.emitter_count];
            const NV_StreamEmitter& x = m.table[row];
            if (x.rebase[0] != 0 || x.rebase[1] != 0 || x.rebase[2] != 0 || (x.flags & (NV_STREAM_EMITTER_TRANSPORT | NV_STREAM_EMITTER_SOURCE | NV_STREAM_EMITTER_KILLED)) ||
                x.parent_event != NV_STREAM_NONE || x.parent_row != NV_STREAM_NONE)
                m.tickRows.push_back(row);
            const uint32_t output = x.program < m.programOutput.size() ? m.programOutput[x.program] : 0u;
            if ((x.flags & NV_STREAM_EMITTER_ACTIVE) && (output == 2u || output == 3u) && !m.outputFlag[row])
            {
                m.outputFlag[row] = 1;
                m.outputRows.push_back(row);
            }
        }
        if (delta && (h.flags & NV_STREAM_PROGRAMS))  // a new program table can change any row's output: rebuild the list
        {
            m.outputRows.clear();
            m.outputFlag.assign(tableRows, 0);
            for (uint32_t row = 0; row < tableRows; ++row)
            {
                const NV_StreamEmitter& x = m.table[row];
                const uint32_t output = x.program < m.programOutput.size() ? m.programOutput[x.program] : 0u;
                if ((x.flags & NV_STREAM_EMITTER_ACTIVE) && (output == 2u || output == 3u)) { m.outputFlag[row] = 1; m.outputRows.push_back(row); }
            }
        }
        // leave the list: rows no longer active output rows
        for (size_t k = 0; k < m.outputRows.size();)
        {
            const uint32_t row = m.outputRows[k];
            const NV_StreamEmitter& x = m.table[row];
            const uint32_t output = x.program < m.programOutput.size() ? m.programOutput[x.program] : 0u;
            if (row < tableRows && (x.flags & NV_STREAM_EMITTER_ACTIVE) && (output == 2u || output == 3u)) { ++k; continue; }
            m.outputFlag[row] = 0;
            m.outputRows[k] = m.outputRows.back();
            m.outputRows.pop_back();
        }
        m.tableRows = tableRows;
        const uint32_t serialNow = ++m.serial;
        m.emitterUpdates.ensure(device, (uint64_t)h.emitter_count * sizeof(NV_StreamEmitter));
        m.emitterUpdateRows.ensure(device, (uint64_t)h.emitter_count * 4);
        m.emitterPatches.ensure(device, (uint64_t)h.emitter_patch_count * sizeof(NV_StreamEmitterPatch));
        m.dynamic[cur].ensure(device, (uint64_t)tableRows * sizeof(EmitterDynamic));
        m.spawns.ensure(device, (uint64_t)h.spawn_count * sizeof(NV_StreamSpawn));
        m.explicitBirths.ensure(device, (uint64_t)h.explicit_count * sizeof(NV_StreamExplicitBirth));
        m.fields.ensure(device, (uint64_t)h.field_count * sizeof(NV_StreamField));
        m.worldFields.ensure(device, (uint64_t)h.world_field_count * sizeof(NV_StreamWorldField));
        if (h.flags & (NV_STREAM_SURFACES | NV_STREAM_RESET))
        {
            m.surfaces.ensure(device, (uint64_t)h.surface_count * sizeof(NV_StreamSurface));
            m.surfaceCount = h.surface_count;
        }
        const uint32_t surfaceTotal = m.surfaceCount + h.dynamic_surface_count;  // static table + this tick's dynamic rows
        // collision surfaces exist only in a simulating packet: a state packet (dt == 0) carries no body frames or dynamic
        // surfaces, so nothing resolves a body index or builds the grid (NativeVfxStream.h, state packets)
        const bool collide = surfaceTotal != 0 && h.dt > 0 && !(m_experimentDisable & 8u);
        m.dynamicSurfaces.ensure(device, (uint64_t)h.dynamic_surface_count * sizeof(NV_StreamSurface));
        m.tickSurfaces.ensure(device, (uint64_t)surfaceTotal * sizeof(NV_StreamSurface));
        // Collision grid: buckets = the power of two >= 2 x surfaces (>= 1024); at most 64 cells per listed surface.
        uint32_t gridBuckets = 1024;
        while (gridBuckets < 2 * surfaceTotal) gridBuckets <<= 1;
        const uint32_t gridEntryCapacity = std::max<uint32_t>(surfaceTotal, 1) * 64;
        m.gridCount.ensure(device, (uint64_t)gridBuckets * 4);
        m.gridStart.ensure(device, (uint64_t)gridBuckets * 4);
        m.gridFill.ensure(device, (uint64_t)gridBuckets * 4);
        m.gridEntries.ensure(device, (uint64_t)gridEntryCapacity * 4);
        m.gridLarge.ensure(device, (uint64_t)std::max<uint32_t>(surfaceTotal, 1) * 4);
        m.surfaceBoxes.ensure(device, (uint64_t)std::max<uint32_t>(surfaceTotal, 1) * 32);
        m.colliders.ensure(device, (uint64_t)std::max<uint32_t>(capacity, 1) * 48);
        m.trace.ensure(device, 528);
        m.overflowRecords.ensure(device, (uint64_t)kOverflowRecords * kOverflowRecordBytes);
        m.ribbonPoints.ensure(device, (uint64_t)h.ribbon_points * 32);
        m.ribbonLinks.ensure(device, (uint64_t)h.ribbon_points * 4);
        m.ribbonVertices.ensure(device, (uint64_t)h.ribbon_points * 64);
        // ribbon ranges of this tick (active NV_RIBBON rows, by output_base) for the strip scan
        struct RibbonRange { uint32_t base, count, program, pad; };
        std::vector<RibbonRange> ribbonRanges;
        {
            for (uint32_t e : m.outputRows)
            {
                const NV_StreamEmitter& x = m.table[e];
                const uint32_t count = x.next_birth - x.death_birth;
                if (!(x.flags & NV_STREAM_EMITTER_ACTIVE) || count == 0 || x.program >= m.programOutput.size() || m.programOutput[x.program] != 2u) continue;
                if ((uint64_t)x.output_base + count > h.ribbon_points) fail("FX particles: ribbon row %u outside the %u ribbon points", e, h.ribbon_points);
                ribbonRanges.push_back({ x.output_base, count, x.program, 0 });
            }
            std::sort(ribbonRanges.begin(), ribbonRanges.end(), [](const RibbonRange& a, const RibbonRange& b) { return a.base < b.base; });
            if (ribbonRanges.size() > 65535u) fail("FX particles: %zu ribbon ranges exceed one dispatch (65535 groups)", ribbonRanges.size());
        }
        const uint32_t ribbonN = ribbonRanges.empty() ? 0 : h.ribbon_points;
        // volume ranges (active NV_VOLUME rows, by first cell): the record index of a volume particle (Particles.hlsli)
        struct VolumeRange { uint32_t first, cells, grid, program, particleBase, pad[3]; };
        std::vector<VolumeRange> volumeRanges;
        for (uint32_t e : m.outputRows)
        {
            const NV_StreamEmitter& x = m.table[e];
            const uint32_t count = x.next_birth - x.death_birth;
            if (!(x.flags & NV_STREAM_EMITTER_ACTIVE) || count == 0 || x.program >= m.programOutput.size() || m.programOutput[x.program] != 3u) continue;
            const uint32_t n = std::clamp<uint32_t>(m.programGrid[x.program], 1u, 32u);
            const uint64_t cells = (uint64_t)count * n * n * n;
            if (x.output_base + cells > h.medium_cells) fail("FX particles: volume row %u outside the %u medium cells", e, h.medium_cells);
            volumeRanges.push_back({ x.output_base, (uint32_t)cells, n, x.program, count, {} });
        }
        std::sort(volumeRanges.begin(), volumeRanges.end(), [](const VolumeRange& a, const VolumeRange& b) { return a.first < b.first; });
        for (size_t k = 1; k < volumeRanges.size(); ++k)
            if (volumeRanges[k].first < volumeRanges[k - 1].first + volumeRanges[k - 1].cells) fail("FX particles: overlapping volume cell ranges at %u", volumeRanges[k].first);
        uint32_t volumeParticles = 0;  // particle bases in first-cell order (the counts ride in particleBase until here)
        for (auto& r : volumeRanges) { const uint32_t c = r.particleBase; r.particleBase = volumeParticles; volumeParticles += c; }
        m.volumeRanges.ensure(device, volumeRanges.size() * 32);
        m.volumeRecords.ensure(device, (uint64_t)volumeParticles * 16);
        m.volumeSide.ensure(device, (uint64_t)volumeParticles * 8);
        m_volumeParticles = volumeParticles;
        m.ribbonRanges.ensure(device, ribbonRanges.size() * 16);
        m.ribbonRunStart.ensure(device, (uint64_t)ribbonN * 4);
        m.ribbonTangents.ensure(device, (uint64_t)ribbonN * 16);
        m.bodies.ensure(device, (uint64_t)h.body_count * sizeof(NV_StreamBody));
        m.restore.ensure(device, (uint64_t)h.restore_count * sizeof(NV_StreamParticle));
        m.slotBase.ensure(device, (uint64_t)h.spawn_count * 4);

        // Birth threads per depth and slot ranks (births that take a slot, in record order; explicit births after the
        // generated births of depth 0).
        const auto* spawns = reinterpret_cast<const NV_StreamSpawn*>(base + h.spawns);
        std::vector<uint32_t> slotBase(std::max<uint32_t>(h.spawn_count, 1));
        uint32_t threads[NV_STREAM_MAX_DEPTH + 1] = {}, slotStart[NV_STREAM_MAX_DEPTH + 2] = {};
        uint32_t rank = 0, explicitSlotBase = 0;
        for (uint32_t d = 0; d <= NV_STREAM_MAX_DEPTH; ++d)
        {
            slotStart[d] = rank;
            for (uint32_t k = h.depth[d]; k < h.depth[d + 1]; ++k)
            {
                if (spawns[k].thread_offset != threads[d]) fail("FX particles: spawn record %u thread_offset %u != prefix %u", k, spawns[k].thread_offset, threads[d]);
                threads[d] += spawns[k].count;
                slotBase[k] = rank;
                rank += spawns[k].count - spawns[k].expired;
            }
            if (d == 0)
            {
                explicitSlotBase = rank;
                rank += h.explicit_count;
            }
        }
        slotStart[NV_STREAM_MAX_DEPTH + 1] = rank;
        if (rank > capacity) fail("FX particles: %u births take slots, capacity %u", rank, capacity);

        // Ring slot of this tick: upload (constants + packet + slot ranks), GPU events, readback. A state packet
        // (dt == 0) without RESET keeps its tick: it reuses that tick's slot, keeps the tick's events and updates only
        // counters.alive and status (NativeVfxStream.h).
        const bool statePacket = h.dt == 0 && !(h.flags & NV_STREAM_RESET);
        int thisSlot = -1;
        if (statePacket)
            for (size_t i = 0; i < m.slots.size(); ++i)
                if (m.slots[i].recorded && m.slots[i].stream == h.stream && m.slots[i].generation == h.generation && m.slots[i].tick == h.tick) thisSlot = (int)i;
        const bool keepEvents = thisSlot >= 0;
        if (!keepEvents)
        {
            thisSlot = (int)m.nextSlot;
            m.nextSlot = (m.nextSlot + 1) % (uint32_t)m.slots.size();
        }
        Impl::Slot& slot = m.slots[(size_t)thisSlot];
        if (slot.recorded) m.waitSlot(device, slot);  // its upload is free; a replaced tick's readback can no longer be read
        const uint64_t constOffset = 0, packetOffset = kConstBytes, rankOffset = align(packetOffset + packet.size(), 16);
        const uint64_t rangesOffset = align(rankOffset + slotBase.size() * 4, 16);
        const uint64_t volumeOffset = align(rangesOffset + ribbonRanges.size() * 16, 16);
        const uint64_t rowsOffset = align(volumeOffset + volumeRanges.size() * 32, 16);
        const uint64_t uploadBytes = rowsOffset + updateRows.size() * 4;
        if (!slot.upload || slot.uploadBytes < uploadBytes)
        {
            if (slot.upload) { slot.upload->Unmap(0, nullptr); device.deferRelease(slot.upload); }
            slot.uploadBytes = std::max<uint64_t>(align(uploadBytes, 65536), slot.uploadBytes * 3 / 2);
            slot.upload = makeBuffer(device, slot.uploadBytes, D3D12_HEAP_TYPE_UPLOAD, L"FX particle upload");
            D3D12_RANGE none{ 0, 0 };
            check(slot.upload->Map(0, &none, reinterpret_cast<void**>(&slot.mapped)), "map FX upload");
        }
        std::memcpy(slot.mapped + packetOffset, packet.data(), packet.size());
        // the tick's fields into the constant buffer (Particles.hlsli FxTick rows) when they fit
        if (h.field_count && h.field_count <= kCbFields)
            std::memcpy(slot.mapped + constOffset + kFieldRowsOffset, base + h.fields, (size_t)h.field_count * sizeof(NV_StreamField));
        if (h.world_field_count && h.world_field_count <= kCbWorldFields)
            std::memcpy(slot.mapped + constOffset + kWorldRowsOffset, base + h.world_fields, (size_t)h.world_field_count * sizeof(NV_StreamWorldField));
        std::memcpy(slot.mapped + rankOffset, slotBase.data(), slotBase.size() * 4);
        if (!ribbonRanges.empty()) std::memcpy(slot.mapped + rangesOffset, ribbonRanges.data(), ribbonRanges.size() * 16);
        if (!volumeRanges.empty()) std::memcpy(slot.mapped + volumeOffset, volumeRanges.data(), volumeRanges.size() * 32);
        std::memcpy(slot.mapped + rowsOffset, updateRows.data(), updateRows.size() * 4);
        // collision events copied with the tick: twice the last count the CPU read, at least 1024 and at most the configured
        // prefix; more are fetched from the tick's event buffer when read (exact either way)
        const uint32_t collisionPrefix = std::min(m_collisionReadback, std::max<uint32_t>(1024, 2 * m.lastCollisions));
        const uint32_t collisionCopy = keepEvents ? slot.collisionCopy : std::min(h.collision_capacity, collisionPrefix);
        if (!keepEvents) slot.events.ensure(device, (uint64_t)(h.event_slots + h.collision_capacity) * sizeof(NV_StreamEvent));
        const uint64_t readbackBytes = kReportBytes + (uint64_t)(h.event_slots + collisionCopy) * sizeof(NV_StreamEvent);
        if (!keepEvents && (!slot.readback || slot.readbackBytes < readbackBytes))
        {
            if (slot.readback) device.deferRelease(slot.readback);
            slot.readbackBytes = std::max<uint64_t>(align(readbackBytes, 4096), slot.readbackBytes * 3 / 2);
            slot.readback = makeBuffer(device, slot.readbackBytes, D3D12_HEAP_TYPE_READBACK, L"FX particle readback");
        }
        slot.recorded = true;
        slot.stream = h.stream;
        slot.generation = h.generation;
        slot.tick = h.tick;
        if (!keepEvents)
        {
            slot.eventSlots = h.event_slots;
            slot.collisionCopy = collisionCopy;
            slot.collisionCapacity = h.collision_capacity;
        }
        m.queue = &device.queue(QueueType::Graphics);
        slot.fenceBase = m.queue->lastSignaled();
        slot.fence = 0;
        m.latestSlot = thisSlot;
        m_latestTick = h.tick;

        // Graph imports of this frame.
        RenderGraph& g = fc.graph;
        std::vector<Buf*> state = { &m.posAge[0], &m.posAge[1], &m.velocity[0], &m.velocity[1], &m.meta, &m.alive, &m.aliveList, &m.deadList, &m.dyingList, &m.blockSums,
                                    &m.spawnedSlots, &m.dynamic[cur], &m.counters,
                                    &m.report, &slot.events, &m.tickSurfaces, &m.gridCount, &m.gridStart, &m.gridFill, &m.gridEntries, &m.gridLarge,
                                    &m.ribbonPoints, &m.ribbonLinks, &m.ribbonVertices, &m.volumeRecords, &m.volumeSide,
                                    &m.ribbonRunStart, &m.ribbonTangents, &m.gridBlocks, &m.surfaceBoxes, &m.overflowRecords, &m.colliders, &m.trace,
                                    &m.emitterTable, &m.emitterStamp };
        std::vector<Buf*> inputs = { &m.programs, &m.curveKeys, &m.emitterUpdates, &m.emitterUpdateRows, &m.emitterPatches, &m.spawns, &m.explicitBirths, &m.fields, &m.worldFields, &m.surfaces,
                                     &m.restore, &m.slotBase, &m.bodies, &m.dynamicSurfaces, &m.ribbonRanges, &m.volumeRanges };
        for (Buf* b : state) b->import(g, fc.frame.frameIndex);
        for (Buf* b : inputs) b->import(g, fc.frame.frameIndex);

        ID3D12Resource* upload = slot.upload.Get();
        const D3D12_GPU_VIRTUAL_ADDRESS constants = upload->GetGPUVirtualAddress() + constOffset;
        uint8_t* constantsCpu = slot.mapped + constOffset;

        // Sort key frame: the main view of this frame, relative to the stream's anchor.
        const ViewDesc& view = fc.frame.mainView;
        float camera[3], forward[3];
        for (int a = 0; a < 3; ++a)
        {
            camera[a] = (float)((double)(&view.position.x)[a] - h.anchor[a]);
            forward[a] = -view.view.m[2][a];
        }
        const float keyNear = std::max(view.nearPlane, 1e-3f);
        const float keyScale = 16777213.0f / std::log2(kKeyFar / keyNear);

        // 1. upload: copies of this tick's sections
        struct Copy { Buf* dst; uint64_t offset, bytes; };
        std::vector<Copy> copies;
        auto add = [&](Buf& dst, uint64_t sectionOffset, uint64_t bytes) {
            if (bytes) copies.push_back({ &dst, packetOffset + sectionOffset, bytes });
        };
        if (h.flags & NV_STREAM_PROGRAMS)
        {
            add(m.programs, h.programs, (uint64_t)h.program_count * sizeof(NV_StreamProgram));
            add(m.curveKeys, h.curve_keys, (uint64_t)h.curve_key_count * 16);
        }
        add(m.emitterUpdates, h.emitters, (uint64_t)h.emitter_count * sizeof(NV_StreamEmitter));
        add(m.emitterPatches, h.emitter_patches, (uint64_t)h.emitter_patch_count * sizeof(NV_StreamEmitterPatch));
        if (h.emitter_count) copies.push_back({ &m.emitterUpdateRows, rowsOffset, (uint64_t)h.emitter_count * 4 });
        add(m.spawns, h.spawns, (uint64_t)h.spawn_count * sizeof(NV_StreamSpawn));
        add(m.explicitBirths, h.explicit_births, (uint64_t)h.explicit_count * sizeof(NV_StreamExplicitBirth));
        add(m.fields, h.fields, (uint64_t)h.field_count * sizeof(NV_StreamField));
        add(m.worldFields, h.world_fields, (uint64_t)h.world_field_count * sizeof(NV_StreamWorldField));
        if (h.flags & (NV_STREAM_SURFACES | NV_STREAM_RESET)) add(m.surfaces, h.surfaces, (uint64_t)h.surface_count * sizeof(NV_StreamSurface));
        add(m.bodies, h.bodies, (uint64_t)h.body_count * sizeof(NV_StreamBody));
        add(m.dynamicSurfaces, h.dynamic_surfaces, (uint64_t)h.dynamic_surface_count * sizeof(NV_StreamSurface));
        add(m.restore, h.restore, (uint64_t)h.restore_count * sizeof(NV_StreamParticle));
        if (h.spawn_count) copies.push_back({ &m.slotBase, rankOffset, (uint64_t)h.spawn_count * 4 });
        if (!ribbonRanges.empty()) copies.push_back({ &m.ribbonRanges, rangesOffset, ribbonRanges.size() * 16 });
        if (!volumeRanges.empty()) copies.push_back({ &m.volumeRanges, volumeOffset, volumeRanges.size() * 32 });
        g.addPass("fx.particles.upload", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      for (const Copy& c : copies) b.use(c.dst->ref, Use::CopyDst);
                      b.keep();
                  },
                  [copies, upload](PassContext& c) {
                      for (const Copy& cp : copies) c.cmd->CopyBufferRegion(c.resource(cp.dst->ref), 0, upload, cp.offset, cp.bytes);
                  });

        // Constants of the tick are written by the first compute pass (it holds every view index).
        TickConstants tc{};
        tc.capacity = capacity;
        tc.numScanBlocks = scanBlocks;
        tc.flags = h.flags;
        tc.fieldCount = h.field_count;
        tc.worldFieldCount = h.world_field_count;
        tc.surfaceCount = collide ? surfaceTotal : 0u;
        tc.staticSurfaceCount = m.surfaceCount;
        tc.emitterCount = tableRows;
        tc.updateCount = h.emitter_count;
        tc.patchCount = h.emitter_patch_count;
        tc.traceRow = m_traceRow;
        tc.traceBirth = m_traceBirth;
        tc.separationMax = m.separationMax;
        tc.experiment = m_experimentDisable;
        if (m_experimentDisable & 16u) tc.fieldCount = tc.worldFieldCount = 0;  // timing attribution only
        tc.volumeRangeCount = (uint32_t)volumeRanges.size();
        tc.serial = serialNow;
        tc.eventSlots = h.event_slots;
        tc.collisionCapacity = h.collision_capacity;
        tc.aliveAfter = h.alive_after;
        tc.restoreCount = h.restore_count;
        tc.tickLo = (uint32_t)h.tick;
        tc.tickHi = (uint32_t)(h.tick >> 32);
        tc.streamLo = (uint32_t)h.stream;
        tc.streamHi = (uint32_t)(h.stream >> 32);
        tc.generationLo = (uint32_t)h.generation;
        tc.generationHi = (uint32_t)(h.generation >> 32);
        tc.recordCount = h.spawn_count;
        tc.explicitCount = h.explicit_count;
        tc.dt = h.dt_float;
        tc.time = h.time_float;
        tc.keyNear = keyNear;
        tc.keyScale = keyScale;
        std::memcpy(tc.camera, camera, 12);
        tc.explicitSlotBase = explicitSlotBase;
        std::memcpy(tc.forward, forward, 12);
        tc.bodyCount = h.body_count;
        tc.gridCell = m_gridCell;
        tc.gridMask = gridBuckets - 1;
        tc.gridEntryCapacity = gridEntryCapacity;
        tc.ribbonCapacity = h.ribbon_points;
        tc.cellCapacity = h.medium_cells;

        auto declare = [state, inputs](PassBuilder& b) {
            for (Buf* x : state) b.use(x->ref, Use::UavCompute);
            for (Buf* x : inputs) b.use(x->ref, Use::SrvCompute);
        };
        auto fillConstants = [&m, cur, constantsCpu, tc, sp = &slot](PassContext& c) mutable {
            tc.posAge = c.uav(m.posAge[cur ^ 1u].ref);
            tc.velocity = c.uav(m.velocity[cur ^ 1u].ref);
            tc.posAgeOut = c.uav(m.posAge[cur].ref);
            tc.velocityOut = c.uav(m.velocity[cur].ref);
            tc.meta = c.uav(m.meta.ref);
            tc.alive = c.uav(m.alive.ref);
            tc.aliveList = c.uav(m.aliveList.ref);
            tc.deadList = c.uav(m.deadList.ref);
            tc.dyingList = c.uav(m.dyingList.ref);
            tc.counters = c.uav(m.counters.ref);
            tc.blockSums = c.uav(m.blockSums.ref);
            tc.events = c.uav(sp->events.ref);
            tc.programs = c.srv(m.programs.ref);
            tc.curveKeys = c.srv(m.curveKeys.ref);
            tc.emitters = c.uav(m.emitterTable.ref);
            tc.emitterUpdates = c.srv(m.emitterUpdates.ref);
            tc.emitterUpdateRows = c.srv(m.emitterUpdateRows.ref);
            tc.emitterPatches = c.srv(m.emitterPatches.ref);
            tc.emitterStamp = c.uav(m.emitterStamp.ref);
            tc.spawns = c.srv(m.spawns.ref);
            tc.explicitBirths = c.srv(m.explicitBirths.ref);
            tc.fields = c.srv(m.fields.ref);
            tc.worldFields = c.srv(m.worldFields.ref);
            tc.surfaces = c.srv(m.surfaces.ref);
            tc.restore = c.srv(m.restore.ref);
            tc.slotBase = c.srv(m.slotBase.ref);
            tc.spawnedSlots = c.uav(m.spawnedSlots.ref);
            tc.report = c.uav(m.report.ref);
            tc.emitterDynamic = c.uav(m.dynamic[cur].ref);
            tc.bodies = c.srv(m.bodies.ref);
            tc.dynamicSurfaces = c.srv(m.dynamicSurfaces.ref);
            tc.tickSurfaces = c.uav(m.tickSurfaces.ref);
            tc.gridCount = c.uav(m.gridCount.ref);
            tc.gridStart = c.uav(m.gridStart.ref);
            tc.gridFill = c.uav(m.gridFill.ref);
            tc.gridEntries = c.uav(m.gridEntries.ref);
            tc.gridLarge = c.uav(m.gridLarge.ref);
            tc.ribbonPoints = c.uav(m.ribbonPoints.ref);
            tc.volumeRecords = c.uav(m.volumeRecords.ref);
            tc.volumeSide = c.uav(m.volumeSide.ref);
            tc.volumeRanges = c.srv(m.volumeRanges.ref);
            tc.surfaceBoxes = c.uav(m.surfaceBoxes.ref);
            tc.colliders = c.uav(m.colliders.ref);
            tc.trace = c.uav(m.trace.ref);
            tc.overflowRecords = c.uav(m.overflowRecords.ref);
            tc.overflowCapacity = kOverflowRecords;
            tc.gridBlocks = c.uav(m.gridBlocks.ref);
            std::memcpy(constantsCpu, &tc, sizeof tc);
        };
        ShaderLibrary& shaders = fc.shaders;
        auto dispatch = [&](const char* name, const char* kernel, std::array<uint32_t, 8> p, uint32_t groupCount, bool first = false) {
            if (groupCount == 0) return;
            ID3D12PipelineState* pso = shaders.compute(kernel);
            g.addPass(name, QueueType::Graphics, declare, [=](PassContext& c) mutable {
                if (first) fillConstants(c);
                c.cmd->SetPipelineState(pso);
                c.bindFrameConstants(constants);
                c.computeConstants(p.data(), 8);
                c.cmd->Dispatch(groupCount, 1, 1);
            });
        };
        auto compaction = [&](const char* suffix, uint32_t checkAlive) {
            dispatch(format("fx.particles.compact.scan%s", suffix).c_str(), "Passes/FX/FxCompact.SCATTER0", {}, scanBlocks);
            dispatch(format("fx.particles.compact.sums%s", suffix).c_str(), "Passes/FX/FxScanSums", { checkAlive }, 1);
            dispatch(format("fx.particles.compact.scatter%s", suffix).c_str(), "Passes/FX/FxCompact.SCATTER1", { checkAlive }, scanBlocks);
        };

        // 2. emitter table (grown table keeps its rows; delta blocks), begin (+ reset and the compaction that rebuilds the
        // dead list)
        if (grownFrom.resource)
        {
            grownFrom.import(g, fc.frame.frameIndex);
            const BufferRef from = grownFrom.ref, to = m.emitterTable.ref;
            const uint64_t bytes = grownFrom.bytes;
            g.addPass("fx.particles.emitters.grow", QueueType::Graphics,
                      [=](PassBuilder& b) {
                          b.use(from, Use::CopySrc);
                          b.use(to, Use::CopyDst);
                      },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(to), 0, c.resource(from), 0, bytes); });
            device.deferRelease(grownFrom.resource);
        }
        dispatch("fx.particles.emitters", "Passes/FX/FxEmitters", {}, std::max<uint32_t>(groups(h.emitter_count + h.emitter_patch_count, 64), 1), true);
        const uint32_t beginThreads = std::max<uint32_t>(std::max<uint32_t>(std::max<uint32_t>(tableRows, 1), reset ? capacity : 0),
                                                         collide ? gridBuckets : 0);  // + grid clear
        dispatch("fx.particles.begin", reset ? "Passes/FX/FxBegin.RESET1" : "Passes/FX/FxBegin.RESET0", {}, groups(beginThreads, 256));
        if (reset) compaction(".reset", 0);
        if (repack)
        {
            Old* o = new Old(old);  // lives until the frame's passes are recorded
            std::shared_ptr<Old> keep(o);
            const char* oldNames[4] = { "fx.old.aliveList", "fx.old.posAge", "fx.old.velocity", "fx.old.meta" };
            Buf* olds[4] = { &keep->list, &keep->posAge, &keep->velocity, &keep->meta };
            for (int k = 0; k < 4; ++k)
            {
                olds[k]->name = oldNames[k];
                olds[k]->import(g, fc.frame.frameIndex);
            }
            ID3D12PipelineState* pso = shaders.compute("Passes/FX/FxRepack");
            g.addPass("fx.particles.repack", QueueType::Graphics,
                      [=](PassBuilder& b) {
                          declare(b);
                          for (int k = 0; k < 4; ++k) b.use(olds[k]->ref, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const std::array<uint32_t, 8> p = { c.uav(keep->list.ref), c.uav(keep->posAge.ref), c.uav(keep->velocity.ref), c.uav(keep->meta.ref),
                                                              0, 0, 0, 0 };
                          c.cmd->SetPipelineState(pso);
                          c.bindFrameConstants(constants);
                          c.computeConstants(p.data(), 8);
                          c.cmd->Dispatch(groups(capacity, 256), 1, 1);
                      });
            compaction(".repack", 0);
        }
        if (collide)
        {
            // grid counts cleared by begin; surfaces = transform + count; one-group scan; fill
            dispatch("fx.particles.surfaces", "Passes/FX/FxSurfaces", {}, groups(surfaceTotal, 64));
            dispatch("fx.particles.grid.scan", "Passes/FX/FxGrid.STEP2", {}, groups(gridBuckets, 1024));
            dispatch("fx.particles.grid.blocks", "Passes/FX/FxGrid.STEP4", {}, 1);
            dispatch("fx.particles.grid.fill", "Passes/FX/FxGrid.STEP3", {}, groups(surfaceTotal, 64));
        }

        // 3. depth 0: spawn (generated + explicit), integrate every slot; depths 1..4: child setup, spawn, integrate
        // (a state packet, dt == 0, has no spawns and no motion: FxIntegrate applies kills only)
        if (h.dt > 0)
            dispatch("fx.particles.spawn.d0", "Passes/FX/FxSpawn", { h.depth[0], h.depth[1], threads[0], threads[0] + h.explicit_count, 1u }, groups(threads[0] + h.explicit_count, 64));
        // live slots of the last compaction + depth-0 births (<= capacity threads; the live count is read on the GPU)
        dispatch("fx.particles.integrate.d0", "Passes/FX/FxIntegrate.LIST0", { h.dt > 0 ? slotStart[1] : 0u }, groups(capacity, 256));
        for (uint32_t d = 1; d <= NV_STREAM_MAX_DEPTH && h.dt > 0; ++d)
        {
            if (h.depth[d + 1] == h.depth[d]) continue;
            const uint32_t spawned = slotStart[d + 1] - slotStart[d];
            dispatch(format("fx.particles.child.d%u", d).c_str(), "Passes/FX/FxChildSetup", { h.depth[d], h.depth[d + 1] }, groups(h.depth[d + 1] - h.depth[d], 64));
            dispatch(format("fx.particles.spawn.d%u", d).c_str(), "Passes/FX/FxSpawn", { h.depth[d], h.depth[d + 1], threads[d], threads[d] }, groups(threads[d], 64));
            dispatch(format("fx.particles.integrate.d%u", d).c_str(), "Passes/FX/FxIntegrate.LIST1", { slotStart[d], spawned }, groups(spawned, 256));
        }

        // collision sweeps of every depth's colliding slots (queued by the integrate passes)
        if (collide) dispatch("fx.particles.collide", "Passes/FX/FxCollide", {}, groups(capacity, 64));

        // 4. compaction (alive / dead / dying lists, report, alive_after check)
        compaction("", 1);


        // ribbon strips: one group per ribbon range walks it in chunks with carried scans (FxRibbon.hlsl)
        if (ribbonN && !(m_experimentDisable & 2u))
        {
            Impl* mi = &m;
            const uint32_t rangeCount = (uint32_t)ribbonRanges.size();
            ID3D12PipelineState* pso = shaders.compute("Passes/FX/FxRibbon");
            g.addPass("fx.particles.ribbon", QueueType::Graphics, declare, [=](PassContext& c) {
                const std::array<uint32_t, 8> p = { c.uav(mi->ribbonPoints.ref), c.uav(mi->ribbonLinks.ref), c.uav(mi->ribbonVertices.ref), c.srv(mi->ribbonRanges.ref),
                                                    c.uav(mi->ribbonRunStart.ref), c.uav(mi->ribbonTangents.ref), rangeCount, 0 };
                c.cmd->SetPipelineState(pso);
                c.bindFrameConstants(constants);
                c.computeConstants(p.data(), 8);
                c.cmd->Dispatch(rangeCount, 1, 1);
            });
        }


        // 5. readback: report + CPU-assigned event slots + a prefix of the collision events
        ID3D12Resource* readback = slot.readback.Get();
        const uint64_t eventBytes = (uint64_t)(h.event_slots + collisionCopy) * sizeof(NV_StreamEvent);
        Buf* reportBuf = &m.report;
        Buf* eventsBuf = &slot.events;
        g.addPass("fx.particles.readback", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(reportBuf->ref, Use::CopySrc);
                      b.use(eventsBuf->ref, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      if (keepEvents)
                      {
                          // NV_StreamCounters.alive (byte 24) and .status (byte 32) of the state packet; the tick's events
                          // and collision count stay
                          c.cmd->CopyBufferRegion(readback, 24, c.resource(reportBuf->ref), 24, 4);
                          c.cmd->CopyBufferRegion(readback, 32, c.resource(reportBuf->ref), 32, 4);
                          return;
                      }
                      c.cmd->CopyBufferRegion(readback, 0, c.resource(reportBuf->ref), 0, kReportBytes);
                      if (eventBytes) c.cmd->CopyBufferRegion(readback, kReportBytes, c.resource(eventsBuf->ref), 0, eventBytes);
                  });
    }
}

TickReadback ParticleSystem::readback(uint64_t stream, uint64_t generation, uint64_t tick)
{
    Impl& m = *m_impl;
    Impl::Slot* found = nullptr;
    for (auto& s : m.slots)
        if (s.recorded && s.stream == stream && s.generation == generation && s.tick == tick) found = &s;
    if (!found) fail("FX particles: tick %llu (stream %llu, generation %llu) is not in the readback ring (not recorded yet, or overwritten)",
                     (unsigned long long)tick, (unsigned long long)stream, (unsigned long long)generation);
    Impl::Slot& s = *found;
    m.waitSlot(m_device, s);
    TickReadback out;
    const uint64_t bytes = kReportBytes + (uint64_t)(s.eventSlots + s.collisionCopy) * sizeof(NV_StreamEvent);
    uint8_t* p = nullptr;
    D3D12_RANGE r{ 0, (SIZE_T)bytes };
    check(s.readback->Map(0, &r, reinterpret_cast<void**>(&p)), "map FX readback");
    std::memcpy(&out.counters, p, sizeof(NV_StreamCounters));
    std::memcpy(&out.dying, p + 48, 4);
    m.lastCollisions = out.counters.collision_events;
    const uint32_t collisions = std::min(out.counters.collision_events, s.collisionCapacity);
    const uint32_t copied = std::min(collisions, s.collisionCopy);
    out.events.resize((size_t)s.eventSlots + collisions);
    std::memcpy(out.events.data(), p + kReportBytes, ((size_t)s.eventSlots + copied) * sizeof(NV_StreamEvent));
    D3D12_RANGE none{ 0, 0 };
    s.readback->Unmap(0, &none);
    if (collisions > copied)
    {
        // The rest of the collision events from the tick's GPU event buffer (kept in the ring slot until reused).
        const uint64_t offset = ((uint64_t)s.eventSlots + copied) * sizeof(NV_StreamEvent), rest = (uint64_t)(collisions - copied) * sizeof(NV_StreamEvent);
        ComPtr<ID3D12Resource> rb = makeBuffer(m_device, rest, D3D12_HEAP_TYPE_READBACK, L"FX collision readback");
        CommandList list = m_device.acquireCommandList(QueueType::Graphics);
        list.list->CopyBufferRegion(rb.Get(), 0, s.events.resource.Get(), offset, rest);
        const uint64_t fence = m_device.submit(list);
        m_device.queue(QueueType::Graphics).waitCpu(fence);
        uint8_t* q = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)rest };
        check(rb->Map(0, &rr, reinterpret_cast<void**>(&q)), "map FX collision readback");
        std::memcpy(out.events.data() + s.eventSlots + copied, q, rest);
        rb->Unmap(0, &none);
    }
    return out;
}

namespace
{
// Copies persistent buffers into readback memory with a one-pass graph of its own (explicit readbacks only).
std::vector<uint8_t> copyOut(Device& device, ID3D12Resource* resource, uint64_t bytes, uint32_t stride, const char* name)
{
    RenderGraph graph(device);
    const BufferRef ref = graph.importBuffer(resource, BufferDesc{ name, std::max<uint64_t>(bytes, 256), stride });
    ComPtr<ID3D12Resource> rb = makeBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK, L"FX state readback");
    graph.addPass("fx.readstate", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(ref, Use::CopySrc);
                      b.keep();
                  },
                  [&](PassContext& c) { c.cmd->CopyBufferRegion(rb.Get(), 0, c.resource(ref), 0, bytes); });
    graph.execute(nullptr);
    device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
    std::vector<uint8_t> out((size_t)bytes);
    uint8_t* p = nullptr;
    D3D12_RANGE r{ 0, (SIZE_T)bytes };
    check(rb->Map(0, &r, reinterpret_cast<void**>(&p)), "map FX state readback");
    std::memcpy(out.data(), p, (size_t)bytes);
    D3D12_RANGE none{ 0, 0 };
    rb->Unmap(0, &none);
    return out;
}
} // namespace

std::vector<uint8_t> ParticleSystem::readState(const char* name)
{
    Impl& m = *m_impl;
    if (m.latestSlot >= 0) m.waitSlot(m_device, m.slots[(size_t)m.latestSlot]);
    const std::string n = name;
    const uint32_t last = m.parity ^ 1u;  // parity of the latest recorded tick
    Buf* b = nullptr;
    uint64_t bytes = (uint64_t)m_capacity * 4;
    if (n == "posAge") b = &m.posAge[last], bytes = (uint64_t)m_capacity * 16;
    else if (n == "velocity") b = &m.velocity[last], bytes = (uint64_t)m_capacity * 16;
    else if (n == "posAgePrev") b = &m.posAge[last ^ 1u], bytes = (uint64_t)m_capacity * 16;  // the renderer's previous tick
    else if (n == "meta") b = &m.meta, bytes = (uint64_t)m_capacity * 8;
    else if (n == "alive") b = &m.alive;
    else if (n == "aliveList") b = &m.aliveList;
    else if (n == "deadList") b = &m.deadList;
    else if (n == "dyingList") b = &m.dyingList;
    else if (n == "counters") b = &m.counters, bytes = kCounterWords * 4;
    else if (n == "emitterDynamic") b = &m.dynamic[last], bytes = m.dynamic[last].bytes;
    else if (n == "ribbonPoints") b = &m.ribbonPoints, bytes = m.ribbonPoints.bytes;
    else if (n == "ribbonVertices") b = &m.ribbonVertices, bytes = m.ribbonVertices.bytes;
    else if (n == "ribbonLinks") b = &m.ribbonLinks, bytes = m.ribbonLinks.bytes;
    else if (n == "volumeRecords") b = &m.volumeRecords, bytes = (uint64_t)m_volumeParticles * 16;
    else if (n == "volumeSide") b = &m.volumeSide, bytes = (uint64_t)m_volumeParticles * 8;
    else if (n == "overflow") b = &m.overflowRecords, bytes = m.overflowRecords.bytes;
    else if (n == "trace") b = &m.trace, bytes = 528;
    else if (n == "tickSurfaces") b = &m.tickSurfaces, bytes = m.tickSurfaces.bytes;
    else fail("FX particles: no state buffer '%s'", name);
    return copyOut(m_device, b->resource.Get(), bytes, b->stride, b->name);
}

std::vector<NV_StreamParticle> ParticleSystem::checkpoint(ShaderLibrary&)
{
    // Every live slot: the alive list names them in slot order; the records come from the state buffers.
    const std::vector<uint8_t> counters = readState("counters");
    uint32_t alive = 0;
    std::memcpy(&alive, counters.data(), 4);
    const std::vector<uint8_t> list = readState("aliveList"), posAge = readState("posAge"), vel = readState("velocity"), meta = readState("meta");
    std::vector<NV_StreamParticle> out(alive);
    for (uint32_t i = 0; i < alive; ++i)
    {
        uint32_t slot;
        std::memcpy(&slot, list.data() + (size_t)i * 4, 4);
        NV_StreamParticle& r = out[i];
        std::memcpy(&r.emitter, meta.data() + (size_t)slot * 8, 4);
        std::memcpy(&r.birth, meta.data() + (size_t)slot * 8 + 4, 4);
        r.reserved0 = 0;
        r.reserved1 = slot;
        std::memcpy(r.position, posAge.data() + (size_t)slot * 16, 12);
        std::memcpy(&r.age, posAge.data() + (size_t)slot * 16 + 12, 4);
        std::memcpy(r.velocity, vel.data() + (size_t)slot * 16, 12);
        r.reserved2 = 0;
    }
    return out;
}

ParticleSystem& particles(TrackState& state, Device& device, const QualityConfig& quality)
{
    auto& holder = state.get<std::unique_ptr<ParticleSystem>>("fx.particles");
    if (!holder) holder = std::make_unique<ParticleSystem>(device, quality);
    return *holder;
}

ParticleSystem* findParticles(TrackState& state) { return state.get<std::unique_ptr<ParticleSystem>>("fx.particles").get(); }
} // namespace unx::fx
