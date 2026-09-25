// unx-kernel: cs_6_6 main
// Collision sweeps of the tick: thread per slot queued by FxIntegrate (every depth) after its motion. Runs
// nv_integrate_finish (the sweep against the tick's surfaces, age += h) and the rest of the slot's tick (fxFinishSlot).
// The queue order (atomic) never changes a result: each slot is independent, the sweep's tie rule fixes the contact
// order, and collision events are an unordered append. After all depths and before the compaction: nothing of a
// later depth reads a collider's result (children come from CPU-assigned birth/death events).
#include "Passes/FX/Particles.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    FX_RWBUFFER(uint, counters, g_counters);
    if (id.x >= min(counters[FX_COUNTER_COLLIDERS], g_capacity)) return;
    FX_RWBUFFER(ColliderRecord, colliders, g_colliders);
    const ColliderRecord c = colliders[id.x];
    if (c.slot >= g_capacity) { fxStatus(FX_STATUS_RANGE); return; }
    FX_RWBUFFER(uint2, meta, g_meta);
    FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const uint2 m = meta[c.slot];
    const uint row = m.x, birth = m.y;
    if (row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    const EmitterDynamic dyn = dynamic[row];
    NvState s;
    s.position = c.start; s.velocity = c.velocity; s.age = c.age;
    fxFinishSlot(c.slot, row, birth, e, p, dyn, fxMotion(p, e, dyn, birth), c.h, c.start, c.move, c.accel, s);
}
