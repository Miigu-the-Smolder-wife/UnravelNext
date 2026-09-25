// unx-kernel: cs_6_6 main
// unx-variants: LIST=0,1
// Integrate (stream step 2 / 3). LIST=0 (depth 0): thread per live slot - the alive list holds the last compaction's
// live slots (slot order) followed by this tick's P[0].x depth-0 births (appended by FxSpawn); dead slots get no thread.
// LIST=1: thread per slot spawned at depth d >= 1 (spawned[P[0].x + t], t < P[0].y).
// Per slot, in the stream's order: an existing slot of a KILLED row dies (no event); an existing slot whose birth is in
// the row's dying range [dying_birth, death_birth) dies, integrated to its lifetime end first when the row writes death
// events (event slot death_event + (birth - dying_birth)), after a. rebase / transport (existing slots), else b-f.
// nv_integrate over h = dt (full-dt drag factors of the row) or, for a new birth (age sign bit), h = elapsed from age 0.
// The first impact of a colliding program with collision events appends a collision event after the CPU slots.
// The motion (nv_integrate_motion) runs here for every slot; a slot of a colliding program is then queued for FxCollide
// (sweep + finish), the others finish here (fxFinishSlot), so the collision sweeps run in waves of colliders only.
// Reads the input state (last tick's output, with this tick's births) and writes this tick's state into the other
// buffer of the pair, so the renderer interpolates the two ticks of a slot from the state itself (no render record copy);
// a dead slot is marked DYING (dying list).
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
    FX_RWBUFFER(uint, counters, g_counters);
    const uint listed = min(counters[FX_COUNTER_ALIVE] + P[0].x, g_capacity);
    if (id.x >= listed) return;
    FX_RWBUFFER(uint, aliveList, g_aliveList);
    const uint slot = aliveList[id.x];
    if (slot >= g_capacity) { fxStatus(FX_STATUS_RANGE); return; }
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
        // State packet (restore, same-tick population change): no motion; the output state is the state.
        FX_RWBUFFER(float4, posAgeOut, g_posAgeOut);
        FX_RWBUFFER(float4, velocityOut, g_velocityOut);
        posAgeOut[slot] = float4(s.position, s.age);
        velocityOut[slot] = float4(s.velocity, 0);
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
    const NvMotion mo = fxMotion(p, e, dyn, birth);
    if (fxTraced(row, birth))
    {
        // diagnostic: the inputs of this particle's integrate call (the end is written by fxFinishSlot)
        FX_RWBUFFER(TraceRecord, trace, g_trace);
        TraceRecord r = (TraceRecord)0;
        r.inputs.row = row; r.inputs.birth = birth; r.inputs.newborn = born ? 1u : 0u; r.inputs.depth = 0u;
        r.inputs.position = s.position; r.inputs.age = s.age; r.inputs.velocity = s.velocity; r.inputs.h = h;
        r.inputs.drag = float4(drag.velocity, drag.position, drag.acceleration, 0);
        r.inputs.emitter = e; r.inputs.dynamic = dyn;
        trace[0] = r;
    }
    float3 start, move, accel;
    nv_integrate_motion(mo, h, drag, s, start, move, accel);
    if (mo.collision != 0u && g_surfaceCount != 0u)
    {
        // the sweep runs in FxCollide, whose waves hold colliding slots only (they no longer stall the other lanes)
        FX_RWBUFFER(uint, counters, g_counters);
        uint at;
        InterlockedAdd(counters[FX_COUNTER_COLLIDERS], 1u, at);
        if (at < g_capacity)
        {
            FX_RWBUFFER(ColliderRecord, colliders, g_colliders);
            ColliderRecord c;
            c.start = start; c.slot = slot; c.move = move; c.h = h; c.velocity = s.velocity; c.age = s.age; c.accel = accel;
            colliders[at] = c;
        }
        else fxStatus(FX_STATUS_CAPACITY);
        return;
    }
    fxFinishSlot(slot, row, birth, e, p, dyn, mo, h, start, move, accel, s);
}
