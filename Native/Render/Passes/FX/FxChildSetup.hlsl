// unx-kernel: cs_6_6 main
// Child emitters of depth d >= 1 (same-tick GPU cascade, user decision 8.1 (b)): thread per spawn record of the depth.
// A row with parent_event gets the event's state: origin_anchor = float(parent origin_anchor + event position) (the
// definition the CPU also uses for the child's double origin next tick) and inherited = ratio (inherited.x of the
// table) x event velocity. Records of the same row write identical values.
// P[0] = (record begin, record end)
#include "Passes/FX/Particles.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint k = P[0].x + id.x;
    if (k >= P[0].y) return;
    FX_BUFFER(StreamSpawn, spawns, g_spawns);
    FX_BUFFER(StreamEmitter, emitters, g_emitters);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    FX_RWBUFFER(StreamEvent, events, g_events);
    const uint row = spawns[k].emitter;
    const StreamEmitter e = emitters[row];
    if (e.parentEvent == FX_NONE) return;
    const StreamEvent ev = events[e.parentEvent];
    EmitterDynamic d;
    d.originAnchor = dynamic[e.parentRow].originAnchor + ev.position;
    d.pad0 = 0u;
    d.inherited = e.inherited.x * ev.velocity;
    d.pad1 = 0u;
    dynamic[row] = d;
}
