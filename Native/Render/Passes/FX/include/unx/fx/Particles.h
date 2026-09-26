#pragma once
// GPU particle module (FX track; WORLD_VFX_DESIGN_KO.md 3, V1). The VFX CPU authority (NativeVfx, Unravel) streams one
// committed packet per tick (NativeVfxStream.h); this module owns every particle's state on the GPU and runs the tick in
// the frame's C0 simulation slot (tracks::simulation) or on the host's simulation queue: restore / spawn + integrate per
// cascade depth / collisions, in a per-row particle layout the CPU computes from the stream's tables (no slots, no
// compaction), then copies the tick's counters and events to a readback ring the CPU reads at its next tick.
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
    uint32_t capacity, inCount, inRangeCount, flags;  // input layout: particles, ranges
    uint32_t fieldCount, worldFieldCount, surfaceCount, emitterCount;
    uint32_t eventSlots, collisionCapacity, aliveAfter, restoreCount;
    uint32_t tickLo, tickHi, streamLo, streamHi;
    uint32_t generationLo, generationHi, recordCount, explicitCount;
    float dt, time; uint32_t outCount, volumeParticles;  // this tick's layout: particles, volume particles (its prefix)
    float reserved27[3]; uint32_t explicitBase;          // birthIndex offset of the explicit births
    float reserved28[3]; uint32_t bodyCount;
    uint32_t posAge, velocity, inRanges, inBlocks;       // posAge/velocity: input state; its layout (InRange, group -> range)
    uint32_t restoreBase, colliderCapacity, reserved30, counters;  // birthIndex offset of the restore records; collider queue
    uint32_t reserved31, posAgeOut, reserved20, events;  // *Out: this tick's state
    uint32_t heightFieldCount, heightFields, heightTiles, reserved24;  // heightfields of the tick (FxHeightField, anchor space), tiles (uint words)
    uint32_t hist, programs, curveKeys, emitters;
    uint32_t spawns, explicitBirths, fields, worldFields;
    uint32_t surfaces, restore, birthIndex, reserved32;  // birthIndex: layout indices of the births (ParticleSystem.cpp)
    uint32_t report, emitterDynamic, bodies, tickSurfaces;
    float gridCell; uint32_t gridMask, gridCount, gridStart;  // collision candidate grid (FxGrid.hlsl)
    uint32_t gridFill, gridEntries, gridLarge, gridEntryCapacity;
    uint32_t staticSurfaceCount, dynamicSurfaces, ribbonPoints, volumeSide;  // surfaceCount = static + dynamic
    uint32_t ribbonCapacity, cellCapacity, volumeRecords, gridBlocks;
    uint32_t emitterUpdates, emitterUpdateRows, emitterStamp, updateCount;
    uint32_t serial;
    float separationMax;  // largest program separation (collision grid motion bound)
    uint32_t volumeRanges, volumeRangeCount;
    uint32_t surfaceBoxes, velocityOut, overflowRecords, overflowCapacity;
    uint32_t reserved25, reserved26, experiment, colliders;  // experiment_disable (timing only); collider queue
    uint32_t emitterPatches, patchCount, traceRow, traceBirth;  // patches of the tick (FxEmitters); traced particle
    uint32_t trace, rowMotion, pad18, pad19;  // TraceRecord buffer (diagnostic, setTrace); RowMotion per row
};
static_assert(sizeof(TickConstants) == 416);

// counters[] words (Particles.hlsli)
enum : uint32_t { kCounterAlive = 0, kCounterDead = 1, kCounterCollisions = 2, kCounterStatus = 3, kCounterDying = 4, kCounterWords = 16 };
// One row of a particle layout (Particles.hlsli "Particle layout"): births [first, first + count) of emitter row 'row' are
// the state at [base, base + count), in birth order.
struct LayoutRange { uint32_t row, base, first, count; };
// Per-row values of a tick (origin anchor, inherited velocity after chain resolution).
struct EmitterDynamic { float originAnchor[3]; uint32_t pad0; float inherited[3]; uint32_t pad1; };
static_assert(sizeof(EmitterDynamic) == 32);

struct TickReadback
{
    NV_StreamCounters counters{};
    std::vector<NV_StreamEvent> events;  // event_slots CPU-assigned records, then the collision events (any order)
    uint32_t dying = 0;                  // particles that died in the tick by their row's dying range (module statistic)
};

// Render input of the latest recorded tick (the particle render pass, ParticleLayer.h): graph imports of both ticks' state,
// dynamic rows and the render ranges (ParticleLayerPass.hlsli RenderRange), and the CPU values of the interpolation.
struct ParticleRenderInputs
{
    render::BufferRef posAge[2], velocity[2], dynamic[2];  // [0] previous tick, [1] latest tick
    render::BufferRef emitters, programs, curveKeys, renderRanges, renderBlocks;
    uint32_t threads = 0, current = 0, rangeCount = 0;     // render threads (current + died in the tick), current
    render::BufferRef ribbonRanges, ribbonRows;            // the render pass's ribbon layout (FxRibbon.hlsl RibbonRange) of the
                                                           // latest tick: per ribbon row births [dying_birth, next_birth) at
                                                           // [base, base + count); rows: uint2 (base, dying_birth) per emitter row
    uint32_t ribbonRangeCount = 0, ribbonCapacity = 0;     // its ranges and points; 0 = none
    double anchor[2][3] = {};                              // stream anchor of each tick's state
    float dt = 0;                                          // dt of the latest tick
    double tickTime = 0;                                   // context time at the end of the latest tick
    bool valid = false;                                    // a tick was recorded
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
    // Readback ring slots: at most this many ticks may be recorded into one graph (a later tick reuses the slot of the
    // tick 'readbackSlots()' before it, which must have executed); a host bounds its pending ticks below it.
    uint32_t readbackSlots() const { return m_readbackSlots; }

    // C0: records every pending tick in order, in the graph of fc (one graphics queue, one list).
    void record(render::FramePassContext& fc);
    // Independent submit (host simulation queue, WORLD_VFX 3.7): records every pending tick into the caller's graph on
    // 'queue'; the caller executes the graph. tickIndex identifies the graph for buffer imports. Readbacks of these ticks
    // wait on that queue's fence.
    void record(render::RenderGraph& graph, render::ShaderLibrary& shaders, uint64_t tickIndex, render::QueueType queue);

    // Queue of the explicit copies (collision events past the readback slot, checkpoint and state reads): Graphics by
    // default; a host whose graphics queue belongs to someone else outside its render event (Unity) sets Compute, since
    // readbacks and checkpoints run on the host's main thread (WORLD_VFX 3.7, I track V3).
    void setExplicitQueue(render::QueueType queue) { m_explicitQueue = queue; }

    // NV_StreamExecutor::readback: counters and events of a recorded tick. Waits for the GPU when the frame that ran it
    // is still executing. Fails (throws) when the tick was never recorded or its ring slot was reused.
    TickReadback readback(uint64_t stream, uint64_t generation, uint64_t tick);

    // NV_StreamExecutor::checkpoint: every live particle after the latest recorded tick, in layout order (waits for the
    // GPU; explicit use only: save, views, CPU projection, tests). reserved1 = its layout index.
    std::vector<NV_StreamParticle> checkpoint(render::ShaderLibrary& shaders);

    // Raw state for tests (waits for the GPU): the bytes of a named buffer ("posAge", "velocity" in layout(); "posAgePrev",
    // "velocityPrev" in layoutPrevious(); "counters", "volumeRecords", "volumeSide", "overflow", "trace", ...).
    std::vector<uint8_t> readState(const char* name);
    // Particle layout of the latest recorded tick (its output) and of its input (the previous tick's output, or the
    // restore records' after RESET): the renderer's interpolation pair (render rules request 2).
    const std::vector<LayoutRange>& layout() const;
    const std::vector<LayoutRange>& layoutPrevious() const;
    // The render pass's inputs, imported into 'graph' (frame 'importIndex'); valid = false before the first tick.
    ParticleRenderInputs renderInputs(render::RenderGraph& graph, uint64_t importIndex);

    uint32_t capacity() const { return m_capacity; }
    // Resident device memory of the module (every default-heap buffer it holds, the ring's event buffers included), bytes.
    // The upload and readback rings are host memory (not counted).
    uint64_t residentBytes() const;
    uint64_t latestTick() const { return m_latestTick; }
    // Diagnostic: every later tick writes the inputs and the end of this particle's integrate call into a TraceRecord
    // (Particles.hlsli; readState("trace"), 544 B). row = UINT32_MAX switches it off.
    void setTrace(uint32_t row, uint32_t birth) { m_traceRow = row; m_traceBirth = birth; }

private:
    void recordPending(render::Device& device, render::RenderGraph& graph, render::ShaderLibrary& shaders, uint64_t importIndex, render::QueueType queue);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    render::Device& m_device;
    uint32_t m_chainDepthMax = 4, m_readbackSlots = 4, m_collisionReadback = 4096;
    float m_gridCell = 1.0f;
    uint32_t m_traceRow = UINT32_MAX, m_traceBirth = 0;
    uint32_t m_volumeParticles = 0;  // live volume particles of the last recorded tick (records)
    uint32_t m_experimentDisable = 0;  // timing attribution only (fx.toml experiment_disable); 0 in every product run
    uint32_t m_capacity = 0;
    uint64_t m_latestTick = 0;
    render::QueueType m_explicitQueue = render::QueueType::Graphics;
    std::deque<std::vector<uint8_t>> m_pending;
};

// The module instance of a renderer (TrackState key "fx.particles"): created on first use.
ParticleSystem& particles(render::TrackState& state, render::Device& device, const QualityConfig& quality);
// The instance if one exists (tracks::simulation records it), else null.
ParticleSystem* findParticles(render::TrackState& state);
} // namespace unx::fx
