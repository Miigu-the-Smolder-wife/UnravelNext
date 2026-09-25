// unx-kernel: cs_6_6 main
// Spawn of one depth (stream step 1 / 3): thread per birth of the spawn records [P[0].x, P[0].y) (thread_offset is the
// exclusive prefix of count inside the depth), then at depth 0 the explicit births. A birth's slot is chosen by its rank
// among the slot-taking births of the tick (slotBase, computed by the module on the CPU): dead[deadCount - 1 - rank],
// so the slot of a particle depends only on the inputs (no atomics). Births with rank < expired take no slot: they
// report their birth event, are integrated for their whole lifetime and report their death event (CPU-assigned slots).
// A new particle stores age = -elapsed (sign bit set, also for 0): the integrate pass of this depth advances it.
// P[0] = (record begin, record end, generated threads, total threads)
#include "Passes/FX/Particles.hlsli"

uint findRecord(uint t, uint begin, uint end)
{
    FX_BUFFER(StreamSpawn, spawns, g_spawns);
    // last record with thread_offset <= t (records of count 0 share their successor's offset)
    uint lo = begin, hi = end;
    while (hi - lo > 1u)
    {
        const uint mid = (lo + hi) >> 1;
        if (spawns[mid].threadOffset <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

void place(uint rank, uint row, uint birth, NvState s, float elapsed)
{
    FX_RWBUFFER(uint, counters, g_counters);
    FX_RWBUFFER(uint, dead, g_deadList);
    const uint deadCount = counters[FX_COUNTER_DEAD];
    if (rank >= deadCount) { fxStatus(FX_STATUS_CAPACITY); return; }
    const uint slot = dead[deadCount - 1u - rank];
    FX_RWBUFFER(float4, posAge, g_posAge);
    FX_RWBUFFER(float4, velocity, g_velocity);
    FX_RWBUFFER(uint2, meta, g_meta);
    FX_RWBUFFER(uint, alive, g_alive);
    FX_RWBUFFER(uint, spawned, g_spawnedSlots);
    posAge[slot] = float4(s.position, asfloat(asuint(elapsed) | 0x80000000u));
    velocity[slot] = float4(s.velocity, 0);
    meta[slot] = uint2(row, birth);
    alive[slot] = FX_SLOT_ALIVE;
    spawned[rank] = slot;
}

void writeEvent(uint index, StreamEvent ev) { fxWriteEvent(index, ev); }

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint t = id.x;
    if (t >= P[0].w) return;
    FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    if (t < P[0].z)
    {
        FX_BUFFER(StreamSpawn, spawns, g_spawns);
        FX_BUFFER(uint, slotBase, g_slotBase);
        const uint k = findRecord(t, P[0].x, P[0].y);
        const StreamSpawn sp = spawns[k];
        const uint r = t - sp.threadOffset;
        const uint row = sp.emitter;
        const StreamEmitter e = emitters[row];
        const StreamProgram p = programs[e.program];
        const EmitterDynamic dyn = dynamic[row];
        const uint birth = sp.firstBirth + r;
        const float elapsed = nv_birth_elapsed(sp.kind, r, sp.interval, sp.carry, sp.rate);
        NvBirthShape b;
        b.shape = p.shape; b.position_radius = p.positionRadius; b.velocity_radius = p.velocityRadius; b.box = p.box;
        b.cone = p.cone; b.cone_cos = p.coneCos; b.velocity = p.velocity; b.speed = e.speed;
        const float fraction = g_dt > 0 ? saturate((g_dt - elapsed) / g_dt) : 1;
        NvState s = nv_birth_state(b, nv_birth_rng(e.rngKey, birth), (e.flags & FX_EMITTER_SOURCE) != 0u ? 1u : 0u,
                                   e.sourcePrevious[0], e.sourcePrevious[1], e.sourcePrevious[2],
                                   e.sourceCurrent[0], e.sourceCurrent[1], e.sourceCurrent[2], e.spawnOffset, dyn.inherited, fraction);
        if (sp.birthEvent != FX_NONE) writeEvent(sp.birthEvent + r, fxEvent(row, birth, FX_EVENT_BIRTH, s));
        if (r < sp.expired)
        {
            // Born and dead inside this tick: its death state is integrated over its whole lifetime.
            if (sp.deathEvent != FX_NONE)
            {
                NvImpact impact;
                nv_integrate(fxMotion(p, e, dyn, birth), p.lifetime, nv_linear_drag(p.drag, p.lifetime), s, impact);
                writeEvent(sp.deathEvent + r, fxEvent(row, birth, FX_EVENT_DEATH, s));
            }
            return;
        }
        place(slotBase[k] + (r - sp.expired), row, birth, s, elapsed);
    }
    else
    {
        FX_BUFFER(StreamExplicitBirth, births, g_explicitBirths);
        const StreamExplicitBirth x = births[t - P[0].z];
        NvState s;
        s.position = x.position; s.velocity = x.velocity; s.age = 0;
        if (x.birthEvent != FX_NONE) writeEvent(x.birthEvent, fxEvent(x.emitter, x.birth, FX_EVENT_BIRTH, s));
        if (x.elapsed >= programs[emitters[x.emitter].program].lifetime)
        {
            // Expires inside this tick: no slot (its rank stays unused), death event over its whole lifetime.
            if (x.deathEvent != FX_NONE)
            {
                const StreamEmitter e = emitters[x.emitter];
                const StreamProgram p = programs[e.program];
                NvImpact impact;
                nv_integrate(fxMotion(p, e, dynamic[x.emitter], x.birth), p.lifetime, nv_linear_drag(p.drag, p.lifetime), s, impact);
                writeEvent(x.deathEvent, fxEvent(x.emitter, x.birth, FX_EVENT_DEATH, s));
            }
            return;
        }
        place(g_explicitSlotBase + (t - P[0].z), x.emitter, x.birth, s, x.elapsed);
    }
}
