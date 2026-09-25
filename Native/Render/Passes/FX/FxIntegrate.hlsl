// unx-kernel: cs_6_6 main
// unx-variants: LIST=0,1
// Integrate (stream step 2 / 3). LIST=0: thread per slot (depth 0: every live slot, existing and newly spawned).
// LIST=1: thread per slot spawned at depth d >= 1 (spawned[P[0].x + t], t < P[0].y).
// Per slot, in the stream's order: an existing slot of a KILLED row dies (no event); an existing slot whose birth is in
// the row's dying range [dying_birth, death_birth) dies, integrated to its lifetime end first when the row writes death
// events (event slot death_event + (birth - dying_birth)), after a. rebase / transport (existing slots), else b-f.
// nv_integrate over h = dt (full-dt drag factors of the row) or, for a new birth (age sign bit), h = elapsed from age 0.
// The first impact of a colliding program with collision events appends a collision event after the CPU slots.
// Writes the state, the tick's render record and the slot's sort key; a dead slot is marked DYING (dying list).
#include "Passes/FX/Particles.hlsli"

void die(uint slot)
{
    FX_RWBUFFER(uint, alive, g_alive);
    alive[slot] = FX_SLOT_DYING;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    FX_RWBUFFER(uint, alive, g_alive);
#if LIST
    if (id.x >= P[0].y) return;
    FX_RWBUFFER(uint, spawned, g_spawnedSlots);
    const uint slot = spawned[P[0].x + id.x];
#else
    const uint slot = id.x;
    if (slot >= g_capacity) return;
#endif
    if (alive[slot] != FX_SLOT_ALIVE) return;
    FX_RWBUFFER(float4, posAge, g_posAge);
    FX_RWBUFFER(float4, velocity, g_velocity);
    FX_RWBUFFER(uint2, meta, g_meta);
    FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const float4 pa = posAge[slot];
    const float4 vv = velocity[slot];
    const uint2 m = meta[slot];
    const uint row = m.x, birth = m.y;
    if (row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); alive[slot] = FX_SLOT_DEAD; return; }
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    const EmitterDynamic dyn = dynamic[row];
    NvState s;
    s.position = pa.xyz; s.velocity = vv.xyz; s.age = pa.w;
    // A KILLED row's slots vanish (no event, no dying record), new births included.
    if ((e.flags & FX_EMITTER_KILLED) != 0u) { alive[slot] = FX_SLOT_DEAD; return; }
    const bool born = fxNegative(pa.w);
    float h;
    NvDrag drag;
    if (g_dt == 0)
    {
        // State packet (restore, same-tick population change): no motion; the key and record describe the state.
        FX_RWBUFFER(RenderRecord, records, g_records);
        RenderRecord rr;
        rr.position = s.position; rr.age = s.age; rr.velocity = s.velocity; rr.emitter = row;
        records[slot] = rr;
        FX_RWBUFFER(uint, keys, g_keyBySlot);
        keys[slot] = fxSortKey(dyn.originAnchor + s.position);
        fxWriteOutputs(slot, birth, s, e, p, dyn);
        return;
    }
    if (born)
    {
        h = -pa.w;
        s.age = 0;
        drag = nv_linear_drag(p.drag, h);
    }
    else
    {
        // a. rebase, transport (every existing slot, dying ones too: event positions are in this tick's origin space)
        s.position -= e.rebase;
        if ((e.flags & FX_EMITTER_TRANSPORT) != 0u)
        {
            s.position = nv_affine_point(e.transport[0], e.transport[1], e.transport[2], s.position);
            s.velocity = nv_affine_vector(e.transport[0], e.transport[1], e.transport[2], s.velocity);
        }
        if (birth - e.dyingBirth < e.deathBirth - e.dyingBirth)
        {
            if ((e.flags & FX_EMITTER_DEATH_EVENTS) != 0u && e.deathEvent != FX_NONE)
            {
                const float rest = max(0.0f, p.lifetime - s.age);
                NvImpact impact;
                nv_integrate(fxMotion(p, e, dyn, birth), rest, nv_linear_drag(p.drag, rest), s, impact);
                fxWriteEvent(e.deathEvent + (birth - e.dyingBirth), fxEvent(row, birth, FX_EVENT_DEATH, s));
            }
            die(slot);
            return;
        }
        h = g_dt;
        drag.velocity = e.dragVelocity; drag.position = e.dragPosition; drag.acceleration = e.dragAcceleration;
    }
    NvImpact impact;
    const bool complete = nv_integrate(fxMotion(p, e, dyn, birth), h, drag, s, impact);
    uint status = complete ? 0u : FX_STATUS_IMPACT_OVERFLOW;
    if (!fxFinite(s))
    {
        fxStatus(status | FX_STATUS_NONFINITE);
        alive[slot] = FX_SLOT_DEAD;  // a defect: removed without a dying record
        return;
    }
    fxStatus(status);
    if (impact.count != 0u && (p.flags & FX_PROGRAM_COLLISION_EVENTS) != 0u)
    {
        FX_RWBUFFER(uint, counters, g_counters);
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
    posAge[slot] = float4(s.position, s.age);
    velocity[slot] = float4(s.velocity, 0);
    FX_RWBUFFER(RenderRecord, records, g_records);
    RenderRecord rr;
    rr.position = s.position; rr.age = s.age; rr.velocity = s.velocity; rr.emitter = row;
    records[slot] = rr;
    FX_RWBUFFER(uint, keys, g_keyBySlot);
    keys[slot] = fxSortKey(dyn.originAnchor + s.position);
    fxWriteOutputs(slot, birth, s, e, p, dyn);
}
