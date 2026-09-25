// unx-kernel: cs_6_6 main
// unx-variants: RESET=0,1
// Tick start of the particle module (after FxEmitters). Thread per emitter row: a row the packet did not send gets its
// per-tick fields cleared; the row's derived values start from the table (child rows
// are overwritten at their depth by FxChildSetup). Thread 0 clears the per-tick counters (collision events, status,
// dying, volume list). RESET=1 (NV_STREAM_RESET): additionally thread per slot: slots [0, restore_count) receive the restore records,
// every other slot becomes dead (the compaction that follows rebuilds the lists).
// P[0].x threads (max(emitters, RESET ? capacity : 0))
#include "Passes/FX/Particles.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i == 0u)
    {
        FX_RWBUFFER(uint, counters, g_counters);
        counters[FX_COUNTER_COLLISIONS] = 0u;
        counters[FX_COUNTER_STATUS] = 0u;
        counters[FX_COUNTER_DYING] = 0u;
        counters[FX_COUNTER_VOLUMES] = 0u;
        counters[FX_COUNTER_OVERFLOWS] = 0u;
        counters[FX_COUNTER_COLLIDERS] = 0u;
        counters[FX_COUNTER_LARGE] = 0u;           // collision grid of the tick (FxSurfaces, FxGrid)
        counters[FX_COUNTER_GRID_ENTRIES] = 0u;
        counters[FX_COUNTER_TURN] = 0u;
        counters[FX_COUNTER_CARRY] = 0u;
    }
    if (i < g_sortPasses * g_histRegion)
    {
        // sort histograms of the tick: counted by the final compaction (pass 0) and each scatter (the next pass)
        FX_RWBUFFER(uint, hist, g_hist);
        hist[i] = 0u;
    }
    if (i <= g_gridMask && g_surfaceCount != 0u)
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
        EmitterDynamic d;
        d.originAnchor = emitters[i].originAnchor; d.pad0 = 0u;
        d.inherited = emitters[i].inherited; d.pad1 = 0u;
        dynamic[i] = d;
    }
#if RESET
    if (i < g_capacity)
    {
        FX_RWBUFFER(float4, posAge, g_posAge);
        FX_RWBUFFER(float4, velocity, g_velocity);
        FX_RWBUFFER(uint2, meta, g_meta);
        FX_RWBUFFER(uint, alive, g_alive);
        if (i < g_restoreCount)
        {
            FX_BUFFER(StreamParticle, restore, g_restore);
            const StreamParticle r = restore[i];
            posAge[i] = float4(r.position, r.age);
            velocity[i] = float4(r.velocity, 0);
            meta[i] = uint2(r.emitter, r.birth);
            alive[i] = FX_SLOT_ALIVE;
        }
        else alive[i] = FX_SLOT_DEAD;
    }
#endif
}
