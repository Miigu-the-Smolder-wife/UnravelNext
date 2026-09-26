// GPU particle module: shared declarations of the FX particle kernels (WORLD_VFX_DESIGN_KO.md 3.3, stream format
// include/unx/fx/NativeVfxStream.h). C++ mirror: include/unx/fx/ParticleGpu.h (sizes asserted on both sides).
//
// Every kernel binds the tick constants (FxTick) as its root CBV b1 and reads/writes the module's buffers through
// their bindless indices. Root constants P[0..1] carry per-dispatch values (depth, ranges, sort pass).
#ifndef FX_PARTICLES_HLSLI
#define FX_PARTICLES_HLSLI
#include "Bindless.hlsli"

#define FX_STATUS_IMPACT_OVERFLOW 1u
#define FX_STATUS_NONFINITE 2u
#define FX_STATUS_ALIVE_MISMATCH 4u
#define FX_STATUS_CAPACITY 8u
#define FX_STATUS_RANGE 16u  // an index outside the packet's declared ranges (write skipped; a defect)
#define FX_STATUS_WATCHDOG 32u  // an enumeration reached its bound (candidates, cells): recorded as a failure, never a
                                // silent truncation; the structure (grid size, lists) must be redesigned, not the bound

// counters[] words (ParticleGpu.h)
#define FX_COUNTER_ALIVE 0u        // particles written into this tick's layout (checked against alive_after on the CPU)
#define FX_COUNTER_DEAD 1u         // (unused: the layout has no dead slots)
#define FX_COUNTER_COLLISIONS 2u
#define FX_COUNTER_STATUS 3u
#define FX_COUNTER_DYING 4u        // (unused: the dying particles are the rows' dying ranges of the input layout)
#define FX_COUNTER_LARGE 5u      // surfaces in the collision grid's large list
#define FX_COUNTER_GRID_ENTRIES 6u
#define FX_COUNTER_VOLUMES 7u        // (unused)
#define FX_COUNTER_TURN 8u           // asuint(max |w| dt over the grid's surfaces) (FxGrid, motion bound of the queries)
#define FX_COUNTER_CARRY 9u          // asuint(max carrier displacement bound over all surfaces)
#define FX_COUNTER_COLLIDERS 11u     // colliding slots queued by integrate for FxCollide this tick
#define FX_COUNTER_OVERFLOWS 10u     // slots of this tick whose sweep needed a fifth impact (diagnostic records)

// Particle layout (row ranges): the state of a tick holds, per live emitter row, the row's live births [death_birth,
// next_birth) in birth order at [base, base + count) (NativeVfxStream.h: the live births of a row are exactly that range),
// the rows one after the other (the CPU chooses the order: volume rows first in first-cell order, so a volume particle's
// record index is its state index). A particle's index is base + (birth - first). Nothing marks slots alive or dead: the
// layout of the tick (computed by the CPU from the stream's tables) is the population.

#define FX_CB_FIELDS 64u        // context fields held in FxTick (C++ kCbFields)
#define FX_CB_WORLD_FIELDS 16u  // world fields held in FxTick (C++ kCbWorldFields)
cbuffer FxTick : register(b1)
{
    uint g_capacity, g_inCount, g_inRangeCount, g_flags;  // input layout: particles (integrate threads), ranges
    uint g_fieldCount, g_worldFieldCount, g_surfaceCount, g_emitterCount;
    uint g_eventSlots, g_collisionCapacity, g_aliveAfter, g_restoreCount;
    uint g_tickLo, g_tickHi, g_streamLo, g_streamHi;
    uint g_generationLo, g_generationHi, g_recordCount, g_explicitCount;
    float g_dt, g_time; uint g_outCount, g_volumeParticles;  // this tick's layout: particles; volume particles (its prefix)
    float3 g_reserved27; uint g_explicitBase;   // birthIndex[g_explicitBase + j]: index of explicit birth j
    float3 g_reserved28; uint g_bodyCount;
    uint g_posAge, g_velocity, g_inRanges, g_inBlocks;  // input state (last tick's output) and its layout (InRange, blocks)
    uint g_restoreBase, g_colliderCapacity, g_reserved30, g_counters;  // birthIndex[g_restoreBase + j]: input index of restore j;
                                                                        // colliders: this tick's particles of colliding rows
    uint g_reserved31, g_posAgeOut, g_reserved20, g_events;
    uint g_heightFieldCount, g_heightFields, g_heightTiles, g_reserved24;  // heightfields of the tick (FxHeightField,
                                                                          // anchor space), their tiles (uint words)
    uint g_hist, g_programs, g_curveKeys, g_emitters;
    uint g_spawns, g_explicitBirths, g_fields, g_worldFields;
    uint g_surfaces, g_restore, g_birthIndex, g_reserved32;  // birthIndex: CPU-computed layout indices (spawn records,
                                                              // explicit births, restore records)
    uint g_report, g_emitterDynamic, g_bodies, g_tickSurfaces;
    float g_gridCell; uint g_gridMask, g_gridCount, g_gridStart;     // collision grid (FxGrid.hlsl)
    uint g_gridFill, g_gridEntries, g_gridLarge, g_gridEntryCapacity;
    uint g_staticSurfaceCount, g_dynamicSurfaces, g_ribbonPoints, g_volumeSide;  // g_surfaceCount = static + dynamic; side8
    uint g_ribbonCapacity, g_cellCapacity, g_volumeRecords, g_gridBlocks;  // header ribbon_points, medium_cells; record16;
                                                                           // grid block offsets
    uint g_emitterUpdates, g_emitterUpdateRows, g_emitterStamp, g_updateCount;  // emitter table delta (FxEmitters.hlsl)
    uint g_serial; float g_separationMax; uint g_volumeRanges, g_volumeRangeCount;  // packet serial; largest separation;
                                                                                  // volume ranges (record index)
    uint g_surfaceBoxes, g_velocityOut, g_overflowRecords, g_overflowCapacity;  // grown box per surface (candidate filter);
                                                                                // this tick's velocity; IMPACT_OVERFLOW inputs
    uint g_reserved25, g_reserved26, g_experiment, g_colliders;  // sort: pass p's histogram is hist[p * g_histRegion + group * 256
                                                                  // + digit]; g_colliders: queue of colliding slots (FxCollide)
    uint g_emitterPatches, g_patchCount, g_traceRow, g_traceBirth;  // patches of the tick (FxEmitters); traced particle
    uint g_trace, g_rowMotion, g_pad18, g_pad19;                     // TraceRecord buffer (diagnostic); RowMotion per row
    // The tick's fields, uniform for every particle: read through the constant path (one broadcast load per row) instead of
    // per-particle buffer loads. Filled by the CPU when the counts fit (else the structured buffers are read).
    uint4 g_fieldRows[FX_CB_FIELDS * 2];             // context fields (StreamField, 32 B each)
    uint4 g_worldFieldRows[FX_CB_WORLD_FIELDS * 4];  // world fields (StreamWorldField, 64 B each)
};

#include "Passes/FX/StreamRecords.hlsli"
// Render record of a slot for one tick (request 20260925_FX_particle_render_rules.md).

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
    NvField r;
    if (g_fieldCount <= FX_CB_FIELDS && (g_experiment & 64u) == 0u)
    {
        const uint4 a = g_fieldRows[2u * i], b = g_fieldRows[2u * i + 1u];
        r.position = asfloat(a.xyz); r.kind = a.w; r.value = asfloat(b.xyz); r.radius = asfloat(b.w);
        return r;
    }
    FX_BUFFER(StreamField, fields, g_fields);
    const StreamField f = fields[i];
    r.position = f.position; r.kind = f.kind; r.value = f.value; r.radius = f.radius;
    return r;
}
NvWorldField fxWorldField(uint i)
{
    StreamWorldField f;
    if (g_worldFieldCount <= FX_CB_WORLD_FIELDS && (g_experiment & 64u) == 0u)
    {
        const uint4 a = g_worldFieldRows[4u * i], b = g_worldFieldRows[4u * i + 1u], c = g_worldFieldRows[4u * i + 2u], d = g_worldFieldRows[4u * i + 3u];
        f.origin = asfloat(a.xyz); f.packed = a.w;
        f.basis0 = asfloat(b.xyz); f.basis1 = asfloat(uint3(b.w, c.x, c.y)); f.basis2 = asfloat(uint3(c.z, c.w, d.x));
        f.value = asfloat(d.yzw);
    }
    else
    {
        FX_BUFFER(StreamWorldField, fields, g_worldFields);
        f = fields[i];
    }
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
// The tick's heightfields (NV_StreamExecutor version 2): anchor-space frames the CPU built from the static bodies (as the
// CPU reference's load_inputs), tiles as the stream sent them (NV_StreamHeightTile: holes[16], heights[17 x 17]).
struct FxHeightField  // 112 B (ParticleSystem.cpp HeightRecord)
{
    uint entity0, entity1, generation0, generation1;
    uint cellsX, cellsZ, firstTile, tilesX;
    uint firstTriangle, pad0, pad1, pad2;
    float3 origin; float pad3;
    float3 axisX; float pad4;
    float3 axisY; float pad5;
    float3 axisZ; float pad6;
};
NvHeightfield fxHeightfield(uint i)
{
    FX_BUFFER(FxHeightField, fields, g_heightFields);
    const FxHeightField f = fields[i];
    NvHeightfield h;
    h.entity0 = f.entity0; h.entity1 = f.entity1; h.generation0 = f.generation0; h.generation1 = f.generation1;
    h.cells_x = f.cellsX; h.cells_z = f.cellsZ; h.first_tile = f.firstTile; h.tiles_x = f.tilesX; h.first_triangle = f.firstTriangle;
    h.origin = f.origin; h.axis_x = f.axisX; h.axis_y = f.axisY; h.axis_z = f.axisZ;
    return h;
}
float fxHeight(uint tile, uint sample)
{
    FX_BUFFER(uint, tiles, g_heightTiles);  // 308 words per tile: holes[16], heights[289], reserved[3]
    return asfloat(tiles[tile * 308u + 16u + sample]);
}
uint fxHeightHoles(uint tile, uint word)
{
    FX_BUFFER(uint, tiles, g_heightTiles);
    return tiles[tile * 308u + word];
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
#define NV_HEIGHTFIELD_COUNT g_heightFieldCount
#define NV_HEIGHTFIELD(i) fxHeightfield(i)
#define NV_HEIGHT(tile, sample) fxHeight(tile, sample)
#define NV_HEIGHT_HOLES(tile, word) fxHeightHoles(tile, word)

// ---- collision candidates (NV_SURFACE_QUERY hook of the shared mathematics) --------------------------------------------
// A spatial hash grid of the tick's surfaces (FxGrid.hlsl): every surface whose inflated AABB spans at most
// FX_GRID_SURFACE_CELLS cells is listed in the buckets of those cells; larger ones are in the large list. A segment's
// candidates are the large list plus the buckets of the cells its AABB spans (hash collisions and duplicates only add
// candidates; the shared tie rule makes the order irrelevant). A segment spanning more than FX_GRID_QUERY_CELLS cells
// tests every surface. So the candidates are always a superset of the surfaces the segment can hit.
//
// Moving surfaces (nv_collide sweeps each surface in its own frame): a surface n with velocity v, angular velocity w
// about its centre of mass o and extent r (largest distance of its points from o) is hit by the relative path from
// P = carry_n(W) to the segment's end E, where W = uncarry_c(A) is the sweep's start A taken back through the motion
// of the carrier c (the surface of the last contact; W = A without one). With theta = |w| h, u = |v| h over the
// interval h <= dt and |W - A| <= D_c = u_c + theta_c (r_c + separation), a hit point x of n and the point y of the
// query segment at the same parameter satisfy
//     |x - y| <= [theta_n r_n + (1 + theta_n) u_n] / (1 - theta_n) + [(1 + theta_n) D_c + theta_n |d|] / (1 - theta_n)
// (|P - A| <= u_n + theta_n |W - o_n + v h| + D_c and |W - o_n| <= D_c + |d| + |x - y| + r_n). The first term grows the
// surface's box (FxGrid); the second grows the query box with the tick's maxima theta_max over the grid's surfaces and
// D_max over all surfaces (FX_COUNTER_TURN / FX_COUNTER_CARRY). A surface with theta >= 1/2 is in the large list (always
// a candidate). Static surfaces add nothing.
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
    float3 lo, hi;    // grown query box (candidates whose grown box misses it are skipped)
    uint mode;        // 0 large list, 1 grid cells, 2 every surface, 3 done
    uint next, end;   // current list range (large list, bucket entries, surfaces)
    int3 a, span;     // cell box
    uint cell, cells; // next cell index, cell count
    uint visited;     // candidates so far (watchdog)
};
// Bucket start: exclusive prefix inside its block of 1024 buckets + the block's offset (FxGrid STEP 2 and 4).
uint fxGridStart(uint b)
{
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, blocks, g_gridBlocks);
    return starts[b] + blocks[b >> 10];
}
// The AABB is inflated by max(1 mm, 1e-5 |coordinate|), so a point the float hit test accepts is inside it, and by the
// surface's motion over the tick (moving surfaces above); a surface turning by >= 1/2 rad in a tick is large.
// Box of a tick surface (with its motion bound); turn = |w| dt, carry = its carrier displacement bound D (Particles.hlsli).
// Returns false when the surface turns by >= 1/2 rad in the tick or its motion is not finite (large list).
bool fxSurfaceBox(StreamSurface s, out float3 lo, out float3 hi, out float turn, out float carry)
{
    // a, b, c are offsets from the anchor-space reference point s.origin (NativeVfxStream.h)
    float r;
    if (s.kind == 0u) { lo = s.a - s.radius; hi = s.a + s.radius; r = length(s.a) + s.radius; }
    else if (s.kind == 1u) { lo = min(s.a, s.b) - s.radius; hi = max(s.a, s.b) + s.radius; r = max(length(s.a), length(s.b)) + s.radius; }
    else { lo = min(s.a, min(s.b, s.c)); hi = max(s.a, max(s.b, s.c)); r = max(length(s.a), max(length(s.b), length(s.c))); }
    lo += s.origin;
    hi += s.origin;
    turn = 0.0f;
    carry = 0.0f;
    float grow = 0.0f;
    const bool moves = any(s.velocity != 0.0f) || any(s.angular != 0.0f);
    bool small = true;
    if (moves)
    {
        turn = length(s.angular) * g_dt;
        const float u = length(s.velocity) * g_dt;
        carry = u + turn * (r + g_separationMax);
        small = isfinite(turn) && isfinite(carry) && turn < 0.5f;
        grow = small ? (turn * r + (1.0f + turn) * u) / (1.0f - turn) : 0.0f;
        if (!isfinite(carry)) carry = asfloat(0x7F800000u);  // +inf: every query becomes exhaustive
    }
    const float3 pad = max(1e-3f, 1e-5f * max(abs(lo), abs(hi))) + grow;
    lo -= pad;
    hi += pad;
    return small;
}

// Cells of a tick surface in the grid: 0 = large list (too many cells, not finite, or no motion bound).
uint fxSurfaceCells(StreamSurface s, out int3 a, out int3 span, out float3 lo, out float3 hi, out float turn, out float carry, out bool bounded)
{
    bounded = fxSurfaceBox(s, lo, hi, turn, carry);
    const uint cells = fxGridBox(lo, hi, FX_GRID_SURFACE_CELLS, a, span);
    return bounded ? cells : 0u;
}
bool fxQueryBucket(inout FxSurfaceQuery q)
{
    // the next cell of the box with a non-empty bucket; false when the box is done
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, counts, g_gridCount);
    [loop] while (q.cell < q.cells)
    {
        const uint b = fxGridHash(fxGridCellOf(q.a, q.span, q.cell)) & g_gridMask;
        q.cell++;
        q.next = fxGridStart(b);
        q.end = q.next + counts[b];
        if (q.end > g_gridEntryCapacity) { fxStatus(FX_STATUS_RANGE); q.end = q.next; }
        if (q.next < q.end) return true;
    }
    return false;
}
FxSurfaceQuery fxSurfaceQuery(float3 p, float3 d)
{
    FxSurfaceQuery q;
    FX_RWBUFFER(uint, counters, g_counters);
    const float turn = asfloat(counters[FX_COUNTER_TURN]), carry = asfloat(counters[FX_COUNTER_CARRY]);
    const float grow = (turn > 0.0f || carry > 0.0f) ? ((1.0f + turn) * carry + turn * length(d)) / (1.0f - turn) : 0.0f;
    q.lo = min(p, p + d) - grow;
    q.hi = max(p, p + d) + grow;
    q.cells = fxGridBox(q.lo, q.hi, FX_GRID_QUERY_CELLS, q.a, q.span);
    q.cell = 0u;
    q.visited = 0u;
    if (q.cells == 0u) { q.mode = 2u; q.next = 0u; q.end = g_surfaceCount; }
    else { q.mode = 0u; q.next = 0u; q.end = min(counters[FX_COUNTER_LARGE], g_surfaceCount); }
    return q;
}
// Watchdog: a legal enumeration visits at most every surface (exhaustive) or the large list plus the grid entries.
// A candidate whose grown box (FxGrid) does not overlap the grown query box cannot be hit (the same bound as the cells,
// without the cell rounding) and is skipped before the shared sweep loads it. A large-list surface without a bound has an
// infinite box.
bool fxSurfaceNext(inout FxSurfaceQuery q, out uint n)
{
    n = 0u;
    FX_RWBUFFER(float4, boxes, g_surfaceBoxes);
    [loop] for (uint guard = 0u; guard < 3u + g_surfaceCount + g_gridEntryCapacity; ++guard)
    {
        if (q.next < q.end)
        {
            if (++q.visited > g_surfaceCount + g_gridEntryCapacity) { fxStatus(FX_STATUS_WATCHDOG); q.mode = 3u; q.next = q.end; return false; }
            if (q.mode == 0u) { FX_RWBUFFER(uint, large, g_gridLarge); n = large[q.next]; }
            else if (q.mode == 1u) { FX_RWBUFFER(uint, entries, g_gridEntries); n = entries[q.next]; }
            else n = q.next;
            q.next++;
            if (n >= g_surfaceCount) { fxStatus(FX_STATUS_RANGE); n = 0u; return true; }
            const float3 blo = boxes[2u * n].xyz, bhi = boxes[2u * n + 1u].xyz;
            if (any(bhi < q.lo) || any(blo > q.hi)) continue;
            return true;
        }
        if (q.mode == 0u) { q.mode = 1u; if (fxQueryBucket(q)) continue; q.mode = 3u; return false; }
        if (q.mode == 1u) { if (fxQueryBucket(q)) continue; q.mode = 3u; return false; }
        q.mode = 3u;
        return false;
    }
    fxStatus(FX_STATUS_WATCHDOG);
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
    if ((g_experiment & 32u) != 0u) m.noise = float3(0, 0, 0);  // timing attribution only (fx.toml experiment_disable)
    m.origin_anchor = dyn.originAnchor;
    return m;
}

// Per-row values of the tick that every slot of the row reads (filled by FxBegin after the per-tick clears): one contiguous
// 112 B record instead of scattered fields of the 336 B emitter row and, through a second dependent load, the 320 B
// program. Exact copies of the same fields, so every result is unchanged.
struct RowMotion
{
    float3 acceleration; float drag;                  // program
    float3 noise; float noiseFrequency;               // program
    float dragVelocity, dragPosition, dragAcceleration, lifetime;  // emitter (full-dt drag factors), program
    uint noiseKey, program, dyingBirth, deathBirth;   // emitter
    float3 rebase; uint deathEvent;                   // emitter (per-tick)
    float restitution, friction, separation; uint flags;  // program; flags: emitter flags | program flags << 8 | output << 24
    uint entity0, entity1, generation0, generation1;  // emitter (self-collision exclusion)
};
#define FX_ROW_PROGRAM_SHIFT 8u
#define FX_ROW_OUTPUT_SHIFT 24u
RowMotion fxRowMotion(StreamEmitter e, StreamProgram p)
{
    RowMotion r;
    r.acceleration = p.acceleration; r.drag = p.drag;
    r.noise = p.noise; r.noiseFrequency = p.noiseFrequency;
    r.dragVelocity = e.dragVelocity; r.dragPosition = e.dragPosition; r.dragAcceleration = e.dragAcceleration; r.lifetime = p.lifetime;
    r.noiseKey = e.noiseKey; r.program = e.program; r.dyingBirth = e.dyingBirth; r.deathBirth = e.deathBirth;
    r.rebase = e.rebase; r.deathEvent = e.deathEvent;
    r.restitution = p.restitution; r.friction = p.friction; r.separation = p.separation;
    r.flags = (e.flags & 0xFFu) | ((p.flags & 0xFFFFu) << FX_ROW_PROGRAM_SHIFT) | ((p.output & 0xFFu) << FX_ROW_OUTPUT_SHIFT);
    r.entity0 = e.entity.x; r.entity1 = e.entity.y; r.generation0 = e.generation.x; r.generation1 = e.generation.y;
    return r;
}
uint fxRowEmitterFlags(RowMotion r) { return r.flags & 0xFFu; }
uint fxRowProgramFlags(RowMotion r) { return (r.flags >> FX_ROW_PROGRAM_SHIFT) & 0xFFFFu; }
uint fxRowOutput(RowMotion r) { return r.flags >> FX_ROW_OUTPUT_SHIFT; }
// NvMotion of a slot from its row's RowMotion (identical to fxMotion(p, e, dyn, birth)).
NvMotion fxMotionRow(RowMotion r, EmitterDynamic dyn, uint birth)
{
    NvMotion m;
    const uint pf = fxRowProgramFlags(r);
    m.acceleration = r.acceleration;
    m.drag = r.drag;
    m.noise = r.noise;
    m.noise_frequency = r.noiseFrequency;
    m.noise_seed = nv_noise_seed(r.noiseKey, birth);
    m.wind = (pf & FX_PROGRAM_WIND) != 0u ? 1u : 0u;
    m.collision = (pf & FX_PROGRAM_COLLISION) != 0u ? 1u : 0u;
    m.self = (pf & FX_PROGRAM_COLLIDE_SELF) != 0u ? 1u : 0u;
    m.entity0 = r.entity0; m.entity1 = r.entity1; m.generation0 = r.generation0; m.generation1 = r.generation1;
    m.restitution = r.restitution; m.friction = r.friction; m.separation = r.separation;
    if ((g_experiment & 32u) != 0u) m.noise = float3(0, 0, 0);  // timing attribution only (fx.toml experiment_disable)
    m.origin_anchor = dyn.originAnchor;
    return m;
}



bool fxFinite(NvState s) { return all(isfinite(s.position)) && all(isfinite(s.velocity)) && isfinite(s.age); }

// Writes a CPU-assigned event slot; an index outside [0, event_slots) is reported, never written.
// One row of the input layout (the last tick's output), in index order: births [first, first + (next base - base)) of
// row at [base, ...); this tick's output range of the row: births [outFirst, outFirst + outCount) at outBase (outCount 0:
// the row has no live particle after the tick). 32 B.
struct InRange { uint base, first, row, outBase; uint outFirst, outCount, pad0, pad1; };
// The input range holding input index i of dispatch group 'group' (256 threads): inBlocks[group] is the range holding the
// group's first index, inBlocks[group + 1] the one holding the next group's, so the search covers one or a few ranges.
InRange fxInRange(uint i, uint group)
{
    FX_BUFFER(uint, blocks, g_inBlocks);
    FX_BUFFER(InRange, ranges, g_inRanges);
    uint lo = blocks[group], hi = min(blocks[group + 1u] + 1u, g_inRangeCount);
    [loop] for (uint guard = 0u; guard < 32u && hi - lo > 1u; ++guard)
    {
        const uint mid = (lo + hi) >> 1;
        if (ranges[mid].base <= i) lo = mid; else hi = mid;
    }
    return ranges[lo];
}
// Counts particles written into this tick's layout (one atomic per wave; FX_COUNTER_ALIVE, checked against alive_after).
void fxCountAlive()
{
    const uint n = WaveActiveCountBits(true);
    if (WaveIsFirstLane())
    {
        FX_RWBUFFER(uint, counters, g_counters);
        InterlockedAdd(counters[FX_COUNTER_ALIVE], n);
    }
}
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

// ---- geometry outputs (WORLD_VFX_DESIGN_KO.md 3.5), written by the integrate kernel with the slot's final state -----------
// The live births of an emitter are [death_birth, next_birth), so a live particle's output index is
// output_base + (birth - death_birth) (NativeVfxStream.h): no sort.
//   NV_RIBBON: ribbon point {origin-space position, width = size (full), age} (strips: FxRibbon.hlsl);
//   NV_VOLUME: grid^3 medium cells (NV_MediumCell, 96 B) from output_base + (birth - death_birth) x grid^3 (output_base
//              counts cells): cuboid of side = size around the particle, separable tent mass over the grid, coefficients
//              = program coefficients x density (colour alpha scales mass, RGB tints emission) / the support volume (the
//              old VfxMediaShader rules, in float). Cell coordinates: integer 1024 m cells of the anchor space + float
//              offsets inside the cell.
struct RibbonPoint { float3 position; float width; float age; uint valid; uint pad0, pad1; };  // 32 B
float fxCurve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }
float3 fxCurve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }
// Inputs of an nv_integrate call whose sweep needed a fifth impact (diagnostic, FxIntegrate): 448 B.
struct OverflowRecord
{
    uint row, birth, newborn, depth;
    float3 position; float age;       // state passed to nv_integrate (after rebase / transport; age 0 for a new birth)
    float3 velocity; float h;         // and its interval
    float4 drag;                      // NvDrag velocity, position, acceleration
    StreamEmitter emitter;            // the row as the kernel read it (per-tick fields included)
    EmitterDynamic dynamic;           // origin_anchor, inherited velocity of this tick
    float4 accel;                     // finish inputs (newborn = 2): effective acceleration of the motion; else 0
};
// Diagnostic trace of one particle (g_traceRow, g_traceBirth): the inputs of its integrate call this tick in the
// OverflowRecord form (position/velocity/age = state before the motion, after rebase/transport; h; drag), then the end:
// state, impact count, the collision event (if any). 448 + 32 + 64 = 544 B.
struct TraceRecord
{
    OverflowRecord inputs;
    float3 endPosition; float endAge;
    float3 endVelocity; uint impacts;
    StreamEvent collision;
};
bool fxTraced(uint row, uint birth) { return g_traceRow != FX_NONE && row == g_traceRow && birth == g_traceBirth; }
// A colliding particle after its motion (nv_integrate_motion), waiting for its sweep in FxCollide: 68 B (index = its
// index in this tick's layout).
struct ColliderRecord { float3 start; uint index; float3 move; float h; float3 velocity; float age; float3 accel; uint row; uint birth; };
// Local volume particles (render rules request 3b; design 14.8 decision 2): the froxel pass evaluates each particle's
// medium directly from two records per live volume particle (no medium cells):
//   record16 = { float3 centre (anchor space), uint half2(r, m) }: the density field is
//              rho(x) = m * prod_i max(0, 1 - |x_i - c_i| / r) / r^3   (a tent of radius r per axis; its integral is m),
//              r = size(u) / 2, m = alpha(u) (u = age / lifetime; the cell totals of the former NV_MediumCell grid);
//   side8    = { uint emission factor mediumEmission * colour(u) as R11G11B10F (HDR, not saturated), uint program (kind) }.
//   The medium of a particle is sigma_a = medium_absorption * rho, sigma_s = medium_scattering * rho (RGB, per kind),
//   emission = side8.emission * rho, phase g per kind: the stream's cell formula (colour(u) tints the emission only).
// Index: the row's particle base (prefix over the tick's volume ranges, CPU) + birth - death_birth; ranges are found from
// the row's first cell (output_base counts cells, NativeVfxStream.h) by a bounded binary search.
struct VolumeRange { uint first, cells, grid, program, particleBase, pad0, pad1, pad2; };  // 32 B
uint fxPackR11G11B10(float3 c)
{
    c = max(c, 0.0f);
    const uint r = (f32tof16(c.r) >> 4) & 0x7FFu, g = (f32tof16(c.g) >> 4) & 0x7FFu, b = (f32tof16(c.b) >> 5) & 0x3FFu;
    return r | (g << 11) | (b << 22);
}
// Integrate epilogue: a ribbon particle writes its point; a volume particle writes its record16 + side8 at its layout
// index (the volume rows are the layout's prefix, in first-cell order: the record index of the render rules request 3b).
void fxWriteOutputs(uint index, uint row, uint birth, NvState s, RowMotion rm, EmitterDynamic dyn)
{
    const uint output = fxRowOutput(rm);
    if (output != FX_OUTPUT_RIBBON && output != FX_OUTPUT_VOLUME) return;
    FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[rm.program];
    if (p.output == FX_OUTPUT_RIBBON)
    {
        const float u = saturate(s.age / p.lifetime);
        const float size = p.size * e.sizeScale * fxCurve1(p.sizeKeys, p.sizeCount, u);
        const uint ribbonIndex = e.outputBase + (birth - e.deathBirth);
        if (ribbonIndex >= g_ribbonCapacity) { fxStatus(FX_STATUS_RANGE); return; }
        FX_RWBUFFER(RibbonPoint, points, g_ribbonPoints);
        RibbonPoint rp;
        rp.position = s.position; rp.width = size; rp.age = s.age; rp.valid = 1u; rp.pad0 = rp.pad1 = 0u;
        points[ribbonIndex] = rp;
    }
    else if (p.output == FX_OUTPUT_VOLUME)
    {
        const float u = saturate(s.age / p.lifetime);
        if (index >= g_volumeParticles) { fxStatus(FX_STATUS_RANGE); return; }
        const float size = p.size * e.sizeScale * fxCurve1(p.sizeKeys, p.sizeCount, u);
        const float mass = p.color.a * e.colorScale.a * fxCurve1(p.alphaKeys, p.alphaCount, u);
        const float3 colour = p.color.rgb * e.colorScale.rgb * fxCurve3(p.colorKeys, p.colorCount, u);
        FX_RWBUFFER(uint4, records, g_volumeRecords);
        FX_RWBUFFER(uint2, side, g_volumeSide);
        records[index] = uint4(asuint(dyn.originAnchor + s.position), f32tof16(0.5f * size) | (f32tof16(mass) << 16));
        side[index] = uint2(fxPackR11G11B10(p.mediumEmission * colour), e.program);
    }
}
// ---- end of a particle's tick (FxIntegrate / FxSpawn for non-colliding particles, FxCollide for colliding ones) ---------
// nv_integrate_finish (the sweep or start + move, age += h) and everything after it: status, the IMPACT_OVERFLOW
// diagnostic record (the finish inputs: position = start, velocity = velocity after the motion, drag.xyz = move,
// accel = effective acceleration, newborn = 2 marks this layout), the collision event, this tick's state at its layout
// index and the outputs.
// FX_FINISH_SWEEP 0 (FxIntegrate, FxSpawn): the kernel finishes non-colliding slots only - a colliding slot reaches it
// only when the tick has no surface and no heightfield (fxStep queues it for FxCollide otherwise), where the sweep finds
// nothing and nv_collide's result is exactly start + move (its no-hit path) - so the sweep is not compiled in.
#ifndef FX_FINISH_SWEEP
#define FX_FINISH_SWEEP 1
#endif
void fxFinishSlot(uint index, uint row, uint birth, RowMotion rm, EmitterDynamic dyn, NvMotion mo, float h, float3 start, float3 move,
                  float3 accel, NvState s)
{
#if !FX_FINISH_SWEEP
    mo.collision = 0u;
#endif
    const NvState before = s;
    NvImpact impact;
    const bool complete = nv_integrate_finish(mo, h, start, move, accel, s, impact);
    uint status = complete ? 0u : FX_STATUS_IMPACT_OVERFLOW;
    FX_RWBUFFER(uint, counters, g_counters);
    if (!complete && g_overflowCapacity != 0u)
    {
        uint at;
        InterlockedAdd(counters[FX_COUNTER_OVERFLOWS], 1u, at);
        if (at < g_overflowCapacity)
        {
            FX_RWBUFFER(OverflowRecord, records, g_overflowRecords);
            OverflowRecord r;
            r.row = row; r.birth = birth; r.newborn = 2u; r.depth = 0u;
            r.position = start; r.age = before.age;
            r.velocity = before.velocity; r.h = h;
            r.drag = float4(move, 0);
            r.accel = float4(accel, 0);
            FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
            r.emitter = emitters[row];
            r.dynamic = dyn;
            records[at] = r;
        }
    }
    FX_RWBUFFER(float4, posAgeOut, g_posAgeOut);
    FX_RWBUFFER(float4, velocityOut, g_velocityOut);
    if (!fxFinite(s))
    {
        // a defect: the index keeps a NaN age (never drawn) and is not counted (alive mismatch)
        fxStatus(status | FX_STATUS_NONFINITE);
        posAgeOut[index] = float4(0, 0, 0, asfloat(0x7FC00000u));
        velocityOut[index] = float4(0, 0, 0, 0);
        return;
    }
    fxStatus(status);
    if (impact.count != 0u && (fxRowProgramFlags(rm) & FX_PROGRAM_COLLISION_EVENTS) != 0u)
    {
        uint n;
        InterlockedAdd(counters[FX_COUNTER_COLLISIONS], 1u, n);
        if (n < g_collisionCapacity)
        {
            FX_RWBUFFER(StreamEvent, events, g_events);
            StreamEvent ev;
            ev.emitter = row; ev.birth = birth; ev.kind = FX_EVENT_COLLISION; ev.impacts = impact.count;
            ev.position = impact.contact; ev.after = (1 - impact.fraction) * h;
            ev.velocity = impact.velocity; ev.reserved0 = 0; ev.normal = impact.normal; ev.reserved1 = 0;
            events[g_eventSlots + n] = ev;
        }
    }
    posAgeOut[index] = float4(s.position, s.age);
    velocityOut[index] = float4(s.velocity, 0);
    fxCountAlive();
    fxWriteOutputs(index, row, birth, s, rm, dyn);
    if (fxTraced(row, birth))
    {
        FX_RWBUFFER(TraceRecord, trace, g_trace);
        TraceRecord r = trace[0];
        r.endPosition = s.position; r.endAge = s.age; r.endVelocity = s.velocity; r.impacts = impact.count;
        StreamEvent ev;
        ev.emitter = row; ev.birth = birth; ev.kind = FX_EVENT_COLLISION; ev.impacts = impact.count;
        ev.position = impact.contact; ev.after = (1 - impact.fraction) * h;
        ev.velocity = impact.velocity; ev.reserved0 = 0; ev.normal = impact.normal; ev.reserved1 = 0;
        r.collision = ev;
        trace[0] = r;
    }
}
// One particle's tick from its state s (after rebase / transport for an existing particle; the birth state with age 0 for
// a new birth) over h with drag factors 'drag': the trace record, the motion (nv_integrate_motion), then either the
// collider queue (a colliding program: FxCollide sweeps and finishes it) or the finish here. index = its index in this
// tick's layout.
void fxStep(uint index, uint row, uint birth, RowMotion rm, EmitterDynamic dyn, NvState s, float h, NvDrag drag, bool born)
{
    const NvMotion mo = fxMotionRow(rm, dyn, birth);
    if (fxTraced(row, birth))
    {
        // diagnostic: the inputs of this particle's integrate call (the end is written by fxFinishSlot)
        FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
        FX_RWBUFFER(TraceRecord, trace, g_trace);
        TraceRecord r = (TraceRecord)0;
        r.inputs.row = row; r.inputs.birth = birth; r.inputs.newborn = born ? 1u : 0u; r.inputs.depth = 0u;
        r.inputs.position = s.position; r.inputs.age = s.age; r.inputs.velocity = s.velocity; r.inputs.h = h;
        r.inputs.drag = float4(drag.velocity, drag.position, drag.acceleration, 0);
        r.inputs.emitter = emitters[row]; r.inputs.dynamic = dyn;
        trace[0] = r;
    }
    float3 start, move, accel;
    nv_integrate_motion(mo, h, drag, s, start, move, accel);
    if (mo.collision != 0u && (g_surfaceCount != 0u || g_heightFieldCount != 0u))
    {
        // the sweep runs in FxCollide, whose waves hold colliding particles only (they no longer stall the other lanes)
        // one atomic per wave for its colliding lanes (the queue order never changes a result)
        FX_RWBUFFER(uint, counters, g_counters);
        const uint lanes = WaveActiveCountBits(true), rank = WavePrefixCountBits(true);
        uint first = 0u;
        if (WaveIsFirstLane()) InterlockedAdd(counters[FX_COUNTER_COLLIDERS], lanes, first);
        const uint at = WaveReadLaneFirst(first) + rank;
        if (at < g_colliderCapacity)
        {
            FX_RWBUFFER(ColliderRecord, colliders, g_colliders);
            ColliderRecord c;
            c.start = start; c.index = index; c.move = move; c.h = h; c.velocity = s.velocity; c.age = s.age; c.accel = accel;
            c.row = row; c.birth = birth;
            colliders[at] = c;
        }
        else fxStatus(FX_STATUS_CAPACITY);
        return;
    }
    fxFinishSlot(index, row, birth, rm, dyn, mo, h, start, move, accel, s);
}
#endif
