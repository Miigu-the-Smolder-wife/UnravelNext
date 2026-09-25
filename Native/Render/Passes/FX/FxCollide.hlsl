// unx-kernel: cs_6_6 main
// Collision sweeps of the tick: thread per particle queued by FxIntegrate / FxSpawn (every depth) after its motion. Runs
// nv_integrate_finish (the sweep against the tick's surfaces, age += h) and the rest of the slot's tick (fxFinishSlot).
// The queue order (atomic) never changes a result: each particle is independent, the sweep's tie rule fixes the contact
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
    if (c.index >= g_outCount || c.row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
    FX_RWBUFFER(RowMotion, rowMotion, g_rowMotion);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const uint row = c.row, birth = c.birth;
    const RowMotion rm = rowMotion[row];
    const EmitterDynamic dyn = dynamic[row];
    NvState s;
    s.position = c.start; s.velocity = c.velocity; s.age = c.age;
    fxFinishSlot(c.index, row, birth, rm, dyn, fxMotionRow(rm, dyn, birth), c.h, c.start, c.move, c.accel, s);
}
