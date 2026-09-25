// unx-kernel: cs_6_6 main
// unx-variants: RESET=0,1
// Tick start of the particle module. Thread per emitter row of the persistent table (NativeVfxStream.h,
// NV_STREAM_EMITTER_DELTA): the row takes the block the packet sends for it (table[row] = block), or applies its patch
// (the per-tick fields flags, next/death/dying birth, death_event, output_base, parent_event, parent_row and rebase of
// the held block; NV_StreamEmitterPatch), or else reads its per-tick fields as absent (cleared). A whole-table packet
// sends every row as a block. The blocks and the patches are sorted by row; rowWindows[g] = (first block, first patch)
// of rows >= 256 g (the CPU's), so a row searches only its group's window. Then the row's derived values of this tick:
// the dynamic row (child rows are overwritten at their depth by FxChildSetup) and RowMotion.
// Thread 0 clears the per-tick counters; threads <= gridMask clear the collision grid. RESET=1 (NV_STREAM_RESET):
// additionally thread per restore record: the record is the particle at its index of the input layout
// (birthIndex[g_restoreBase + j], computed by the CPU from the records).
// Threads: max(table rows, RESET ? restore_count : 0, grid buckets)
#include "Passes/FX/Particles.hlsli"

// index of 'row' in the sorted keys [lo, hi) of a window, FX_NONE if absent
uint findBlock(uint row, uint lo, uint hi)
{
    FX_BUFFER(uint, rows, g_emitterUpdateRows);
    [loop] for (uint guard = 0u; guard < 32u && lo < hi; ++guard)
    {
        const uint mid = (lo + hi) >> 1, r = rows[mid];
        if (r == row) return mid;
        if (r < row) lo = mid + 1u; else hi = mid;
    }
    return FX_NONE;
}
uint findPatch(uint row, uint lo, uint hi)
{
    FX_BUFFER(StreamEmitterPatch, patches, g_emitterPatches);
    [loop] for (uint guard = 0u; guard < 32u && lo < hi; ++guard)
    {
        const uint mid = (lo + hi) >> 1, r = patches[mid].row;
        if (r == row) return mid;
        if (r < row) lo = mid + 1u; else hi = mid;
    }
    return FX_NONE;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    if (i == 0u)
    {
        FX_RWBUFFER(uint, counters, g_counters);
        counters[FX_COUNTER_ALIVE] = 0u;
        counters[FX_COUNTER_COLLISIONS] = 0u;
        counters[FX_COUNTER_STATUS] = 0u;
        counters[FX_COUNTER_DYING] = 0u;
        counters[FX_COUNTER_GRID_NODES] = 0u;
        counters[FX_COUNTER_OVERFLOWS] = 0u;
        counters[FX_COUNTER_COLLIDERS] = 0u;
        if (g_traceRow != FX_NONE)
        {
            FX_RWBUFFER(TraceRecord, trace, g_trace);
            TraceRecord r = trace[0];
            r.inputs.row = FX_NONE;  // the traced particle's record of this tick (absent until integrate writes it)
            trace[0] = r;
        }
        counters[FX_COUNTER_LARGE] = 0u;           // collision grid of the tick (FxSurfaces)
        counters[FX_COUNTER_GRID_ENTRIES] = 0u;
        counters[FX_COUNTER_TURN] = 0u;
        counters[FX_COUNTER_CARRY] = 0u;
    }
    if (i <= g_gridMask && g_surfaceCount != 0u)
    {
        FX_RWBUFFER(uint, counts, g_gridCount);
        FX_RWBUFFER(uint, heads, g_gridHeads);
        counts[i] = 0u;
        heads[i] = FX_NONE;
    }
    if (i < g_emitterCount)
    {
        FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
        FX_BUFFER(uint2, windows, g_rowWindows);
        const uint2 w0 = windows[gid.x], w1 = windows[gid.x + 1u];
        StreamEmitter e;
        const uint block = findBlock(i, w0.x, w1.x);
        if (block != FX_NONE)
        {
            FX_BUFFER(StreamEmitter, updates, g_emitterUpdates);
            e = updates[block];
            emitters[i] = e;
        }
        else
        {
            e = emitters[i];
            const uint patch = findPatch(i, w0.y, w1.y);
            if (patch != FX_NONE)
            {
                FX_BUFFER(StreamEmitterPatch, patches, g_emitterPatches);
                const StreamEmitterPatch p = patches[patch];
                e.flags = p.flags;
                e.nextBirth = p.nextBirth; e.deathBirth = p.deathBirth; e.dyingBirth = p.dyingBirth; e.deathEvent = p.deathEvent;
                e.outputBase = p.outputBase; e.parentEvent = p.parentEvent; e.parentRow = p.parentRow;
                e.rebase = p.rebase;
                emitters[i] = e;
            }
            else if (any(e.rebase != float3(0, 0, 0)) || (e.flags & (FX_EMITTER_TRANSPORT | FX_EMITTER_SOURCE | FX_EMITTER_KILLED)) != 0u ||
                     e.parentEvent != FX_NONE || e.parentRow != FX_NONE)
            {
                // a row not sent this tick: its per-tick fields are absent (NativeVfxStream.h, NV_STREAM_EMITTER_DELTA)
                e.rebase = float3(0, 0, 0);
                e.flags &= ~(FX_EMITTER_TRANSPORT | FX_EMITTER_SOURCE | FX_EMITTER_KILLED);
                e.parentEvent = FX_NONE;
                e.parentRow = FX_NONE;
                emitters[i] = e;
            }
        }
        FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
        EmitterDynamic d;
        d.originAnchor = e.originAnchor; d.pad0 = 0u;
        d.inherited = e.inherited; d.pad1 = 0u;
        dynamic[i] = d;
        // the row's values of this tick, read by every particle of the row (Particles.hlsli RowMotion)
        FX_BUFFER(StreamProgram, programs, g_programs);
        FX_RWBUFFER(RowMotion, rowMotion, g_rowMotion);
        rowMotion[i] = fxRowMotion(e, programs[e.program]);
    }
#if RESET
    if (i < g_restoreCount)
    {
        FX_RWBUFFER(float4, posAge, g_posAge);
        FX_RWBUFFER(float4, velocity, g_velocity);
        FX_BUFFER(StreamParticle, restore, g_restore);
        FX_BUFFER(uint, birthIndex, g_birthIndex);
        const StreamParticle r = restore[i];
        const uint index = birthIndex[g_restoreBase + i];
        if (index >= g_inCount) return;  // (the CPU validated the indices; thread 0 clears the status in this pass)
        posAge[index] = float4(r.position, r.age);
        velocity[index] = float4(r.velocity, 0);
    }
#endif
}
