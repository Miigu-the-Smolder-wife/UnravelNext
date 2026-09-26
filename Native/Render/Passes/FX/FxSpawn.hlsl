// unx-kernel: cs_6_6 main
// Spawn of one depth (stream steps 1 / 3): thread per birth of the spawn records [P[0].x, P[0].y) (thread_offset is the
// exclusive prefix of count inside the depth), then at depth 0 the explicit births. Births with rank < expired take no
// place in the layout: they report their birth event, are integrated for their whole lifetime and report their death
// event (CPU-assigned slots). Every other birth is integrated here from age 0 over its elapsed time (the stream's
// "integrate the slots spawned at depth d", fxStep) and written at its index in this tick's layout: birthIndex[record] + r
// for a generated birth (the CPU's index of the record's first birth), birthIndex[g_explicitBase + j] for an explicit
// one (Particles.hlsli "Particle layout"). A birth of a KILLED row is not placed (FX_NONE).
// P[0] = (record begin, record end, generated threads, total threads)
#define FX_FINISH_SWEEP 0  // colliding births go to FxCollide (fxFinishSlot)
#include "Passes/FX/Particles.hlsli"

uint findRecord(uint t, uint begin, uint end)
{
    FX_BUFFER(StreamSpawn, spawns, g_spawns);
    // last record with thread_offset <= t (records of count 0 share their successor's offset)
    uint lo = begin, hi = end;
    [loop] for (uint guard = 0u; guard < 32u && hi - lo > 1u; ++guard)  // halves each step: 32 bounds any uint range
    {
        const uint mid = (lo + hi) >> 1;
        if (spawns[mid].threadOffset <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

// Integrates a new birth from age 0 over 'elapsed' and writes it at 'index' of this tick's layout.
void place(uint index, uint row, uint birth, NvState s, float elapsed)
{
    if (index == FX_NONE) return;  // a KILLED row's birth vanishes (NativeVfxStream.h: new births included)
    if (index >= g_outCount) { fxStatus(FX_STATUS_RANGE); return; }
    FX_RWBUFFER(RowMotion, rowMotion, g_rowMotion);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const RowMotion rm = rowMotion[row];
    if ((fxRowEmitterFlags(rm) & FX_EMITTER_KILLED) != 0u) return;
    s.age = 0;
    fxOrientBirth(index, row, birth, rm, elapsed);  // mesh particles (MeshOrientation.hlsli): before the step
    fxStep(index, row, birth, rm, dynamic[row], s, elapsed, nv_linear_drag(rm.drag, elapsed), true);
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
    // a birth that dies inside this tick: its death state is integrated over its whole lifetime (one call site for the
    // generated and the explicit births: the sweep is inlined once)
    uint deathSlot = FX_NONE, deathRow = 0u, deathBirth = 0u;
    NvState deathState = (NvState)0;
    if (t < P[0].z)
    {
        FX_BUFFER(StreamSpawn, spawns, g_spawns);
        FX_BUFFER(uint, birthIndex, g_birthIndex);
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
            // Born and dead inside this tick: no slot, the death event below.
            if (sp.deathEvent != FX_NONE) { deathSlot = sp.deathEvent + r; deathRow = row; deathBirth = birth; deathState = s; }
        }
        else
        {
            const uint first = birthIndex[k];
            place(first == FX_NONE ? FX_NONE : first + r, row, birth, s, elapsed);
        }
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
            // Expires inside this tick: no slot (its rank stays unused), the death event below.
            if (x.deathEvent != FX_NONE) { deathSlot = x.deathEvent; deathRow = x.emitter; deathBirth = x.birth; deathState = s; }
        }
        else
        {
            FX_BUFFER(uint, birthIndex, g_birthIndex);
            place(birthIndex[g_explicitBase + (t - P[0].z)], x.emitter, x.birth, s, x.elapsed);
        }
    }
    if (deathSlot != FX_NONE)
    {
        const StreamEmitter e = emitters[deathRow];
        const StreamProgram p = programs[e.program];
        NvImpact impact;
        nv_integrate(fxMotion(p, e, dynamic[deathRow], deathBirth), p.lifetime, nv_linear_drag(p.drag, p.lifetime), deathState, impact);
        writeEvent(deathSlot, fxEvent(deathRow, deathBirth, FX_EVENT_DEATH, deathState));
    }
}
