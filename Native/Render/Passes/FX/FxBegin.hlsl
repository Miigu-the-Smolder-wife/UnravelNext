// unx-kernel: cs_6_6 main
// unx-variants: RESET=0,1
// Tick start of the particle module (after FxEmitters). Thread per emitter row: a row the packet did not send gets its
// per-tick fields cleared; the row's derived values start from the table (child rows
// are overwritten at their depth by FxChildSetup). Thread 0 clears the per-tick counters (collision events, status,
// dying, volume list, alive). RESET=1 (NV_STREAM_RESET): additionally thread per restore record: the record is the
// particle at its index of the input layout (birthIndex[g_restoreBase + j], computed by the CPU from the records).
// P[0].x threads (max(emitters, RESET ? restore_count : 0, grid buckets))
#include "Passes/FX/Particles.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i == 0u)
    {
        FX_RWBUFFER(uint, counters, g_counters);
        counters[FX_COUNTER_ALIVE] = 0u;
        counters[FX_COUNTER_COLLISIONS] = 0u;
        counters[FX_COUNTER_STATUS] = 0u;
        counters[FX_COUNTER_DYING] = 0u;
        counters[FX_COUNTER_VOLUMES] = 0u;
        counters[FX_COUNTER_OVERFLOWS] = 0u;
        counters[FX_COUNTER_COLLIDERS] = 0u;
        if (g_traceRow != FX_NONE)
        {
            FX_RWBUFFER(TraceRecord, trace, g_trace);
            TraceRecord r = trace[0];
            r.inputs.row = FX_NONE;  // the traced particle's record of this tick (absent until integrate writes it)
            trace[0] = r;
        }
        counters[FX_COUNTER_LARGE] = 0u;           // collision grid of the tick (FxSurfaces, FxGrid)
        counters[FX_COUNTER_GRID_ENTRIES] = 0u;
        counters[FX_COUNTER_TURN] = 0u;
        counters[FX_COUNTER_CARRY] = 0u;
    }
    if (i <= g_gridMask && (g_surfaceCount != 0u || g_heightFieldCount != 0u))
    {
        FX_RWBUFFER(uint, counts, g_gridCount);
        FX_RWBUFFER(uint, fills, g_gridFill);
        counts[i] = 0u;
        fills[i] = 0u;
    }
    if (i < g_emitterCount)
    {
        FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
        FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
        FX_RWBUFFER(uint, stamp, g_emitterStamp);
        if (stamp[i] != g_serial)
        {
            // a row not sent this tick: its per-tick fields are absent (NativeVfxStream.h, NV_STREAM_EMITTER_DELTA)
            emitters[i].rebase = float3(0, 0, 0);
            emitters[i].flags &= ~(FX_EMITTER_TRANSPORT | FX_EMITTER_SOURCE | FX_EMITTER_KILLED);
            emitters[i].parentEvent = FX_NONE;
            emitters[i].parentRow = FX_NONE;
        }
        const StreamEmitter e = emitters[i];
        EmitterDynamic d;
        d.originAnchor = e.originAnchor; d.pad0 = 0u;
        d.inherited = e.inherited; d.pad1 = 0u;
        dynamic[i] = d;
        {
            // the row's values of this tick, read by every slot of the row (Particles.hlsli RowMotion)
            FX_BUFFER(StreamProgram, programs, g_programs);
            FX_RWBUFFER(RowMotion, rowMotion, g_rowMotion);
            rowMotion[i] = fxRowMotion(e, programs[e.program]);
        }
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
        fxOrientRestore(i, index);
    }
#endif
}
