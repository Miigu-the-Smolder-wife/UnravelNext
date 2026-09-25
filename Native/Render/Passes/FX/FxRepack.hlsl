// unx-kernel: cs_6_6 main
// Slot capacity change without RESET (NativeVfxStream.h: slot_capacity follows the need of each tick): the live slots
// move into the new buffers in alive-list order (new slot i = i-th live slot), the rest of the new slots are dead; the
// previous tick's state moves with its slots. Thread per new
// slot. The compaction that follows rebuilds the lists. Identity (emitter, birth) and state are copied bit for bit.
// The state moved is the last tick's output (this tick's input), which is also the renderer's previous tick, so the
// interpolation pairs move with their slots.
// P[0] = (old alive list, old posAge, old velocity, old meta); the live count is the last compaction's (counters)
#include "Passes/FX/Particles.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= g_capacity) return;
    FX_RWBUFFER(uint, alive, g_alive);
    FX_RWBUFFER(uint, counters, g_counters);
    const uint live = counters[FX_COUNTER_ALIVE];
    if (i >= live) { alive[i] = FX_SLOT_DEAD; return; }
    FX_RWBUFFER(uint, oldList, P[0].x);
    FX_RWBUFFER(float4, oldPosAge, P[0].y);
    FX_RWBUFFER(float4, oldVelocity, P[0].z);
    FX_RWBUFFER(uint2, oldMeta, P[0].w);
    FX_RWBUFFER(float4, posAge, g_posAge);
    FX_RWBUFFER(float4, velocity, g_velocity);
    FX_RWBUFFER(uint2, meta, g_meta);
    const uint s = oldList[i];
    posAge[i] = oldPosAge[s];
    velocity[i] = oldVelocity[s];
    meta[i] = oldMeta[s];
    alive[i] = FX_SLOT_ALIVE;
}
