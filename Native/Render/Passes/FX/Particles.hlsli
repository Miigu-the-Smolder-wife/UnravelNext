// GPU particle module: shared declarations of the FX particle kernels (WORLD_VFX_DESIGN_KO.md 3.3, stream format
// include/unx/fx/NativeVfxStream.h). C++ mirror: include/unx/fx/ParticleGpu.h (sizes asserted on both sides).
//
// Every kernel binds the tick constants (FxTick) as its root CBV b1 and reads/writes the module's buffers through
// their bindless indices. Root constants P[0..1] carry per-dispatch values (depth, ranges, sort pass).
#ifndef FX_PARTICLES_HLSLI
#define FX_PARTICLES_HLSLI
#include "Bindless.hlsli"

#define FX_NONE 0xFFFFFFFFu

// Stream enums (NativeVfxStream.h)
#define FX_RESET 1u
#define FX_PROGRAM_NOISE 1u
#define FX_PROGRAM_WIND 2u
#define FX_PROGRAM_COLLISION 4u
#define FX_PROGRAM_COLLIDE_SELF 8u
#define FX_PROGRAM_COLLISION_EVENTS 16u
#define FX_PROGRAM_BIRTH_EVENTS 32u
#define FX_PROGRAM_DEATH_EVENTS 64u
#define FX_PROGRAM_ATTACHED 128u
#define FX_EMITTER_ACTIVE 1u
#define FX_EMITTER_KILLED 2u
#define FX_EMITTER_TRANSPORT 4u
#define FX_EMITTER_SOURCE 8u
#define FX_EMITTER_DEATH_EVENTS 16u
#define FX_EVENT_BIRTH 0u
#define FX_EVENT_DEATH 1u
#define FX_EVENT_COLLISION 2u
#define FX_STATUS_IMPACT_OVERFLOW 1u
#define FX_STATUS_NONFINITE 2u
#define FX_STATUS_ALIVE_MISMATCH 4u
#define FX_STATUS_CAPACITY 8u
#define FX_STATUS_RANGE 16u  // an index outside the packet's declared ranges (write skipped; a defect)
#define FX_STATUS_WATCHDOG 32u  // an enumeration reached its bound (candidates, cells): recorded as a failure, never a
                                // silent truncation; the structure (grid size, lists) must be redesigned, not the bound

// counters[] words (ParticleGpu.h)
#define FX_COUNTER_ALIVE 0u
#define FX_COUNTER_DEAD 1u
#define FX_COUNTER_COLLISIONS 2u
#define FX_COUNTER_STATUS 3u
#define FX_COUNTER_DYING 4u
#define FX_COUNTER_LARGE 5u      // surfaces in the collision grid's large list
#define FX_COUNTER_GRID_ENTRIES 6u

// alive[] values: 0 dead, 1 alive, 2 died in this tick (dying list; compaction clears it to 0)
#define FX_SLOT_DEAD 0u
#define FX_SLOT_ALIVE 1u
#define FX_SLOT_DYING 2u

cbuffer FxTick : register(b1)
{
    uint g_capacity, g_numScanBlocks, g_numSortGroups, g_flags;
    uint g_fieldCount, g_worldFieldCount, g_surfaceCount, g_emitterCount;
    uint g_eventSlots, g_collisionCapacity, g_aliveAfter, g_restoreCount;
    uint g_tickLo, g_tickHi, g_streamLo, g_streamHi;
    uint g_generationLo, g_generationHi, g_recordCount, g_explicitCount;
    float g_dt, g_time, g_keyNear, g_keyScale;
    float3 g_camera; uint g_explicitSlotBase;   // camera - anchor (float): sort key origin
    float3 g_forward; uint g_bodyCount;
    uint g_posAge, g_velocity, g_meta, g_alive;
    uint g_aliveList, g_deadList, g_dyingList, g_counters;
    uint g_blockSums, g_records, g_keyBySlot, g_events;
    uint g_keysA, g_valsA, g_keysB, g_valsB;
    uint g_hist, g_programs, g_curveKeys, g_emitters;
    uint g_spawns, g_explicitBirths, g_fields, g_worldFields;
    uint g_surfaces, g_restore, g_slotBase, g_spawnedSlots;
    uint g_report, g_emitterDynamic, g_bodies, g_tickSurfaces;
    float g_gridCell; uint g_gridMask, g_gridCount, g_gridStart;     // collision grid (FxGrid.hlsl)
    uint g_gridFill, g_gridEntries, g_gridLarge, g_gridEntryCapacity;
    uint g_staticSurfaceCount, g_dynamicSurfaces, g_pad3, g_pad4;  // g_surfaceCount = static + dynamic
};

// ---- stream records (StructuredBuffer layouts: 4-byte packing, same order as NativeVfxStream.h) ------------------
struct StreamProgram  // 320 B
{
    uint output, material, flags, shape;
    float lifetime, drag, positionRadius, velocityRadius;
    float3 acceleration; float size;
    float3 velocity; float coneCos;
    float3 cone; float noiseFrequency;
    float3 box; float reserved0;
    float3 noise; float reserved1;
    float4 color;
    float restitution, friction, separation, reserved2;
    uint sizeKeys, sizeCount, colorKeys, colorCount;
    uint alphaKeys, alphaCount, rotationKeys, rotationCount;
    float4 uv;
    float2 uvScroll; float framesPerSecond, reserved3;
    uint columns, rows, firstFrame, mediumGrid;
    float refractionAmplitude, refractionWidth; uint refractionProfile, reserved4;
    float3 mediumAbsorption; float mediumPhase;
    float3 mediumScattering; float ribbonUv;
    float3 mediumEmission; float ribbonBreak;
    float3 ribbonNormal; float reserved5;
    uint4 reserved6;
};
struct StreamEmitter  // 336 B
{
    uint2 origin[3];                 // double world origin (renderer; the GPU never uses it)
    uint program, flags;
    float3 originAnchor; uint rngKey;
    float3 rebase; uint noiseKey;
    uint nextBirth, deathBirth, dyingBirth, deathEvent;
    float speed, sizeScale, dragVelocity, dragPosition;
    float dragAcceleration, reserved0, reserved1, reserved2;
    float4 colorScale;
    float3 inherited; uint parentEvent;
    float4 transport[3];
    float4 sourcePrevious[3];
    float4 sourceCurrent[3];
    float3 spawnOffset; uint parentRow;
    uint2 entity; uint2 generation;
    uint outputBase, reserved3, reserved4, reserved5;
};
struct StreamSpawn  // 48 B
{
    uint emitter, count, firstBirth, expired;
    uint kind, threadOffset, birthEvent, deathEvent;
    float interval, carry, rate, reserved0;
};
struct StreamExplicitBirth  // 48 B
{
    uint emitter, birth, birthEvent, deathEvent;
    float3 position; float elapsed;
    float3 velocity; float reserved0;
};
struct StreamField { float3 position; uint kind; float3 value; float radius; };  // 32 B
struct StreamWorldField { float3 origin; uint packed; float3 basis0; float3 basis1; float3 basis2; float3 value; };  // 64 B
struct StreamSurface  // 128 B (body == FX_NONE: anchor space; else a, b, c body-local)
{
    uint kind; uint2 entity; uint generation0;
    uint generation1; float radius; uint body, reserved1;
    float3 a; float reserved2; float3 b; float reserved3; float3 c; float reserved4;
    float3 velocity; float reserved5; float3 angular; float reserved6; float3 origin; float reserved7;
};
struct StreamBody { float4 rotation; float3 position; float reserved0; float3 velocity; float reserved1; float3 angular; float reserved2; float3 center; float reserved3; };  // 80 B
struct StreamParticle { uint emitter, birth, reserved0, reserved1; float3 position; float age; float3 velocity; float reserved2; };  // 48 B
struct StreamEvent { uint emitter, birth, kind, impacts; float3 position; float after; float3 velocity; float reserved0; float3 normal; float reserved1; };  // 64 B

// ---- module records ---------------------------------------------------------------------------------------------------
// Emitter values the GPU derives in the tick: every row starts from the table (FxBegin); a child row (parent_event)
// gets origin = float(parent origin_anchor + event position) and inherited = ratio x event velocity at its depth.
struct EmitterDynamic { float3 originAnchor; uint pad0; float3 inherited; uint pad1; };  // 32 B
// Render record of a slot for one tick (request 20260925_FX_particle_render_rules.md).
struct RenderRecord { float3 position; float age; float3 velocity; uint emitter; };  // 32 B

// ---- buffers ----------------------------------------------------------------------------------------------------------
#define FX_BUFFER(T, name, index) StructuredBuffer<T> name = ResourceDescriptorHeap[index]
#define FX_RWBUFFER(T, name, index) RWStructuredBuffer<T> name = ResourceDescriptorHeap[index]

void fxStatus(uint bits)
{
    if (bits == 0u) return;
    FX_RWBUFFER(uint, counters, g_counters);
    InterlockedOr(counters[FX_COUNTER_STATUS], bits);
}

bool fxNegative(float x) { return (asuint(x) >> 31) != 0u; }  // sign bit (a new birth's age, including -0.0)

// ---- hooks of the shared mathematics (VfxParticleMath.hlsli) -----------------------------------------------------------
#define NV_PARTICLE_MATH_TYPES_ONLY
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"
#undef NV_PARTICLE_MATH_TYPES_ONLY

NvField fxField(uint i)
{
    FX_BUFFER(StreamField, fields, g_fields);
    const StreamField f = fields[i];
    NvField r;
    r.position = f.position; r.kind = f.kind; r.value = f.value; r.radius = f.radius;
    return r;
}
NvWorldField fxWorldField(uint i)
{
    FX_BUFFER(StreamWorldField, fields, g_worldFields);
    const StreamWorldField f = fields[i];
    NvWorldField r;
    r.origin = f.origin;
    r.quantity = f.packed & 0xFFu; r.shape = (f.packed >> 8) & 0xFFu; r.operation = (f.packed >> 16) & 0xFFu;
    r.basis0 = f.basis0; r.basis1 = f.basis1; r.basis2 = f.basis2; r.value = f.value;
    return r;
}
// The tick's surfaces in anchor space (FxSurfaces resolves body-local surfaces with this tick's body frames).
NvSurface fxSurface(uint i)
{
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    const StreamSurface s = surfaces[i];
    NvSurface r;
    r.kind = s.kind; r.entity0 = s.entity.x; r.entity1 = s.entity.y; r.generation0 = s.generation0; r.generation1 = s.generation1;
    r.radius = s.radius; r.a = s.a; r.b = s.b; r.c = s.c; r.velocity = s.velocity; r.angular = s.angular; r.origin = s.origin;
    return r;
}
float4 fxCurveKey(uint i)
{
    FX_BUFFER(float4, keys, g_curveKeys);
    return keys[i];
}
#define NV_FIELD_COUNT g_fieldCount
#define NV_FIELD(i) fxField(i)
#define NV_WORLD_FIELD_COUNT g_worldFieldCount
#define NV_WORLD_FIELD(i) fxWorldField(i)
#define NV_SURFACE_COUNT g_surfaceCount
#define NV_SURFACE(i) fxSurface(i)
#define NV_CURVE_KEY(i) fxCurveKey(i)

// ---- collision candidates (NV_SURFACE_QUERY hook of the shared mathematics) --------------------------------------------
// A spatial hash grid of the tick's surfaces (FxGrid.hlsl): every surface whose inflated AABB spans at most
// FX_GRID_SURFACE_CELLS cells is listed in the buckets of those cells; larger ones are in the large list. A segment's
// candidates are the large list plus the buckets of the cells its AABB spans (hash collisions and duplicates only add
// candidates; the shared tie rule makes the order irrelevant). A segment spanning more than FX_GRID_QUERY_CELLS cells
// tests every surface. So the candidates are always a superset of the surfaces the segment can hit.
#define FX_GRID_SURFACE_CELLS 64u
#define FX_GRID_QUERY_CELLS 64u
uint fxGridHash(int3 c) { return ((uint)c.x * 73856093u) ^ ((uint)c.y * 19349663u) ^ ((uint)c.z * 83492791u); }
int3 fxGridCell(float3 p) { return (int3)floor(p / g_gridCell); }
// Cells of the box [lo, hi] (anchor space): 0 when the box is not finite or spans more than 'limit' cells (the caller
// then takes its exhaustive path). Counting first keeps every cell loop bounded, whatever the coordinates.
uint fxGridBox(float3 lo, float3 hi, uint limit, out int3 a, out int3 span)
{
    a = int3(0, 0, 0);
    span = int3(0, 0, 0);
    const float3 cl = floor(lo / g_gridCell), ch = floor(hi / g_gridCell);
    if (!all(isfinite(cl)) || !all(isfinite(ch)) || any(abs(cl) > 1.0e9f) || any(abs(ch) > 1.0e9f)) return 0u;
    const float3 extent = ch - cl + 1.0f;
    if (any(extent < 1.0f) || extent.x * extent.y * extent.z > (float)limit) return 0u;
    a = (int3)cl;
    span = (int3)extent;
    return (uint)(span.x * span.y * span.z);
}
int3 fxGridCellOf(int3 a, int3 span, uint k) { return a + int3((int)(k % (uint)span.x), (int)((k / (uint)span.x) % (uint)span.y), (int)(k / (uint)(span.x * span.y))); }
struct FxSurfaceQuery
{
    uint mode;        // 0 large list, 1 grid cells, 2 every surface, 3 done
    uint next, end;   // current list range (large list, bucket entries, surfaces)
    int3 a, span;     // cell box
    uint cell, cells; // next cell index, cell count
    uint visited;     // candidates so far (watchdog)
};
bool fxQueryBucket(inout FxSurfaceQuery q)
{
    // the next cell of the box with a non-empty bucket; false when the box is done
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, counts, g_gridCount);
    [loop] while (q.cell < q.cells)
    {
        const uint b = fxGridHash(fxGridCellOf(q.a, q.span, q.cell)) & g_gridMask;
        q.cell++;
        q.next = starts[b];
        q.end = q.next + counts[b];
        if (q.end > g_gridEntryCapacity) { fxStatus(FX_STATUS_RANGE); q.end = q.next; }
        if (q.next < q.end) return true;
    }
    return false;
}
FxSurfaceQuery fxSurfaceQuery(float3 p, float3 d)
{
    FxSurfaceQuery q;
    q.cells = fxGridBox(min(p, p + d), max(p, p + d), FX_GRID_QUERY_CELLS, q.a, q.span);
    q.cell = 0u;
    q.visited = 0u;
    FX_RWBUFFER(uint, counters, g_counters);
    if (q.cells == 0u) { q.mode = 2u; q.next = 0u; q.end = g_surfaceCount; }
    else { q.mode = 0u; q.next = 0u; q.end = min(counters[FX_COUNTER_LARGE], g_surfaceCount); }
    return q;
}
// Watchdog: a legal enumeration visits at most every surface (exhaustive) or the large list plus the grid entries.
bool fxSurfaceNext(inout FxSurfaceQuery q, out uint n)
{
    n = 0u;
    [loop] for (uint guard = 0u; guard < 3u; ++guard)
    {
        if (q.next < q.end)
        {
            if (++q.visited > g_surfaceCount + g_gridEntryCapacity) { fxStatus(FX_STATUS_WATCHDOG); q.mode = 3u; q.next = q.end; return false; }
            if (q.mode == 0u) { FX_RWBUFFER(uint, large, g_gridLarge); n = large[q.next]; }
            else if (q.mode == 1u) { FX_RWBUFFER(uint, entries, g_gridEntries); n = entries[q.next]; }
            else n = q.next;
            q.next++;
            if (n >= g_surfaceCount) { fxStatus(FX_STATUS_RANGE); n = 0u; }
            return true;
        }
        if (q.mode == 0u) { q.mode = 1u; if (fxQueryBucket(q)) continue; q.mode = 3u; return false; }
        if (q.mode == 1u) { if (fxQueryBucket(q)) continue; q.mode = 3u; return false; }
        q.mode = 3u;
        return false;
    }
    return false;
}
#define NV_SURFACE_QUERY_TYPE FxSurfaceQuery
#define NV_SURFACE_QUERY(p, d) fxSurfaceQuery(p, d)
#define NV_SURFACE_NEXT(q, n) fxSurfaceNext(q, n)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

// Motion parameters of one slot (NvMotion of the shared mathematics).
NvMotion fxMotion(StreamProgram p, StreamEmitter e, EmitterDynamic dyn, uint birth)
{
    NvMotion m;
    m.acceleration = p.acceleration;
    m.drag = p.drag;
    m.noise = p.noise;  // the amplitudes decide (nv_integrate skips zero noise); the program flag is informational
    m.noise_frequency = p.noiseFrequency;
    m.noise_seed = nv_noise_seed(e.noiseKey, birth);
    m.wind = (p.flags & FX_PROGRAM_WIND) != 0u ? 1u : 0u;
    m.collision = (p.flags & FX_PROGRAM_COLLISION) != 0u ? 1u : 0u;
    m.self = (p.flags & FX_PROGRAM_COLLIDE_SELF) != 0u ? 1u : 0u;
    m.entity0 = e.entity.x; m.entity1 = e.entity.y; m.generation0 = e.generation.x; m.generation1 = e.generation.y;
    m.restitution = p.restitution; m.friction = p.friction; m.separation = p.separation;
    m.origin_anchor = dyn.originAnchor;
    return m;
}

// 24-bit sort key: back-to-front view depth (request 20260925_FX_particle_render_rules.md, option A). Log mapping from
// g_keyNear; points behind the camera sort last.
uint fxSortKey(float3 anchorPosition)
{
    const float depth = dot(anchorPosition - g_camera, g_forward);
    if (!(depth > g_keyNear)) return depth > 0 ? 0xFFFFFEu : 0xFFFFFFu;
    const float code = min(log2(depth / g_keyNear) * g_keyScale, 16777213.0);
    return 0xFFFFFDu - (uint)code;
}


bool fxFinite(NvState s) { return all(isfinite(s.position)) && all(isfinite(s.velocity)) && isfinite(s.age); }

// Writes a CPU-assigned event slot; an index outside [0, event_slots) is reported, never written.
void fxWriteEvent(uint index, StreamEvent ev)
{
    if (index >= g_eventSlots) { fxStatus(FX_STATUS_RANGE); return; }
    FX_RWBUFFER(StreamEvent, events, g_events);
    events[index] = ev;
}

StreamEvent fxEvent(uint emitter, uint birth, uint kind, NvState s)
{
    StreamEvent ev;
    ev.emitter = emitter; ev.birth = birth; ev.kind = kind; ev.impacts = 0u;
    ev.position = s.position; ev.after = 0; ev.velocity = s.velocity; ev.reserved0 = 0; ev.normal = float3(0, 0, 0); ev.reserved1 = 0;
    return ev;
}
#endif
