#pragma once
// GPU particle module (FX track; WORLD_VFX_DESIGN_KO.md 3, V1). The VFX CPU authority (NativeVfx, Unravel) streams one
// committed packet per tick (NativeVfxStream.h); this module owns every particle's state on the GPU and runs the tick in
// the frame's C0 simulation slot (tracks::simulation): reset / spawn / integrate per cascade depth / compaction / sort,
// then copies the tick's counters and events to a readback ring the CPU reads at its next tick.
//
// Threading: submit, readback and checkpoint are called by the host thread between frames (the same thread that records
// frames); record runs inside FrameRenderer::record.
#include "NativeVfxStream.h"
#include "unx/render/Frame.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace unx::fx
{
// Per-tick constants (root CBV b1 of every particle kernel), mirror of cbuffer FxTick in Particles.hlsli.
struct TickConstants
{
    uint32_t capacity, numScanBlocks, numSortGroups, flags;
    uint32_t fieldCount, worldFieldCount, surfaceCount, emitterCount;
    uint32_t eventSlots, collisionCapacity, aliveAfter, restoreCount;
    uint32_t tickLo, tickHi, streamLo, streamHi;
    uint32_t generationLo, generationHi, recordCount, explicitCount;
    float dt, time, keyNear, keyScale;
    float camera[3]; uint32_t explicitSlotBase;
    float forward[3]; uint32_t bodyCount;
    uint32_t posAge, velocity, meta, alive;
    uint32_t aliveList, deadList, dyingList, counters;
    uint32_t blockSums, records, keyBySlot, events;
    uint32_t keysA, valsA, keysB, valsB;
    uint32_t hist, programs, curveKeys, emitters;
    uint32_t spawns, explicitBirths, fields, worldFields;
    uint32_t surfaces, restore, slotBase, spawnedSlots;
    uint32_t report, emitterDynamic, bodies, tickSurfaces;
    float gridCell; uint32_t gridMask, gridCount, gridStart;  // collision candidate grid (FxGrid.hlsl)
    uint32_t gridFill, gridEntries, gridLarge, gridEntryCapacity;
    uint32_t staticSurfaceCount, dynamicSurfaces, ribbonPoints, mediumCells;  // surfaceCount = static + dynamic
    uint32_t ribbonCapacity, cellCapacity, volumeList, gridBlocks;
    uint32_t emitterUpdates, emitterUpdateRows, emitterStamp, updateCount;
    uint32_t serial;
    float separationMax;  // largest program separation (collision grid motion bound)
    uint32_t volumeRanges, volumeRangeCount;
    uint32_t surfaceBoxes, pad8, pad9, pad10;
};
static_assert(sizeof(TickConstants) == 368);

// counters[] words (Particles.hlsli)
enum : uint32_t { kCounterAlive = 0, kCounterDead = 1, kCounterCollisions = 2, kCounterStatus = 3, kCounterDying = 4, kCounterWords = 16 };
// Render record of a slot for one tick (request 20260925_FX_particle_render_rules.md).
struct RenderRecord { float position[3], age, velocity[3]; uint32_t emitter; };
static_assert(sizeof(RenderRecord) == 32);
struct EmitterDynamic { float originAnchor[3]; uint32_t pad0; float inherited[3]; uint32_t pad1; };
static_assert(sizeof(EmitterDynamic) == 32);

struct TickReadback
{
    NV_StreamCounters counters{};
    std::vector<NV_StreamEvent> events;  // event_slots CPU-assigned records, then the collision events (any order)
    uint32_t dying = 0;                  // slots that died in the tick (module statistic)
};

// Per-tick GPU timing breakdown for the gate (pass name prefixes).
class ParticleSystem
{
public:
    ParticleSystem(render::Device& device, const QualityConfig& quality);
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // NV_StreamExecutor::submit: validates and copies a committed packet; the tick runs in the next record().
    void submit(const uint8_t* packet, uint64_t bytes);
    // Ticks submitted and not yet recorded.
    size_t pendingTicks() const { return m_pending.size(); }

    // C0: records every pending tick in order, in the graph of fc (one graphics queue, one list).
    void record(render::FramePassContext& fc);

    // NV_StreamExecutor::readback: counters and events of a recorded tick. Waits for the GPU when the frame that ran it
    // is still executing. Fails (throws) when the tick was never recorded or its ring slot was reused.
    TickReadback readback(uint64_t stream, uint64_t generation, uint64_t tick);

    // NV_StreamExecutor::checkpoint: every live slot after the latest recorded tick (waits for the GPU; explicit use only:
    // save, views, CPU projection, tests).
    std::vector<NV_StreamParticle> checkpoint(render::ShaderLibrary& shaders);

    // Raw state for tests (waits for the GPU): the bytes of a named buffer ("posAge", "velocity", "meta", "alive",
    // "aliveList", "deadList", "dyingList", "counters", "keysSorted", "valsSorted", "records", "keyBySlot").
    std::vector<uint8_t> readState(const char* name);

    uint32_t capacity() const { return m_capacity; }
    uint64_t latestTick() const { return m_latestTick; }
    uint32_t sortPasses() const { return m_sortPasses; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    render::Device& m_device;
    uint32_t m_sortPasses = 3, m_chainDepthMax = 4, m_readbackSlots = 4, m_collisionReadback = 4096;
    float m_gridCell = 1.0f;
    uint32_t m_experimentDisable = 0;  // timing attribution only (fx.toml experiment_disable); 0 in every product run
    uint32_t m_capacity = 0;
    uint64_t m_latestTick = 0;
    std::deque<std::vector<uint8_t>> m_pending;
};

// The module instance of a renderer (TrackState key "fx.particles"): created on first use.
ParticleSystem& particles(render::TrackState& state, render::Device& device, const QualityConfig& quality);
// The instance if one exists (tracks::simulation records it), else null.
ParticleSystem* findParticles(render::TrackState& state);
} // namespace unx::fx
