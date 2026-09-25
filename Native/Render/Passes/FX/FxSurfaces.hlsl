// unx-kernel: cs_6_6 main
// Collision surfaces of the tick (NV_StreamSurface): thread per surface. Surface n < static count is row n of the
// persistent static table, the others are this tick's dynamic rows (static count + dynamic index is the surface number
// of the shared tie rule, NativeVfxStream.h). A tick surface is an anchor-space reference point `origin` and small
// offsets a, b, c from it. A surface on a rigid body (body != NONE) is body-local: origin = the body's centre of mass,
// offsets = R(q) p_local + (position - center) (all terms of body size, so the float precision does not depend on the
// distance from the anchor), and its velocity field is the body's; the other surfaces are copied.
// The same thread then enters the surface into the collision grid's counts (the counts were cleared by FxBegin): its
// grown box (candidate filter), the tick's motion maxima, and either the large list or count[bucket of each cell] += 1.
// Quaternion rotation as the CPU reference: t = 2 cross(u, v), v + w t + cross(u, t).
#include "Passes/FX/Particles.hlsli"

float3 rotateQ(float4 q, float3 v)
{
    const float3 t = cross(q.xyz, v) * 2.0f;
    return v + t * q.w + cross(q.xyz, t);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_surfaceCount) return;
    FX_BUFFER(StreamSurface, table, g_surfaces);
    FX_BUFFER(StreamSurface, dynamicRows, g_dynamicSurfaces);
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    StreamSurface s;
    if (id.x < g_staticSurfaceCount) s = table[id.x];
    else s = dynamicRows[id.x - g_staticSurfaceCount];
    if (s.body != FX_NONE)
    {
        FX_BUFFER(StreamBody, bodies, g_bodies);
        const StreamBody b = bodies[s.body];
        const float3 shift = b.position - b.center;
        s.a = rotateQ(b.rotation, s.a) + shift;
        s.b = rotateQ(b.rotation, s.b) + shift;
        s.c = rotateQ(b.rotation, s.c) + shift;
        s.velocity = b.velocity;
        s.angular = b.angular;
        s.origin = b.center;  // reference point of the offsets and centre of the velocity field
    }
    surfaces[id.x] = s;

    FX_RWBUFFER(uint, counts, g_gridCount);
    FX_RWBUFFER(uint, counters, g_counters);
    int3 a, span;
    float3 lo, hi;
    float turn, carry;
    bool bounded;
    const uint cells = fxSurfaceCells(s, a, span, lo, hi, turn, carry, bounded);
    const bool small = cells != 0u;
    {
        // the candidate filter's box (infinite for a surface without a motion bound)
        FX_RWBUFFER(float4, boxes, g_surfaceBoxes);
        const float inf = asfloat(0x7F800000u);
        boxes[2u * id.x] = float4(bounded ? lo : -inf.xxx, 0);
        boxes[2u * id.x + 1u] = float4(bounded ? hi : inf.xxx, 0);
    }
    // motion maxima of the tick (non-negative floats order as their bits; max is order independent)
    if (carry > 0.0f) InterlockedMax(counters[FX_COUNTER_CARRY], asuint(carry));
    if (small && turn > 0.0f) InterlockedMax(counters[FX_COUNTER_TURN], asuint(turn));
    if (!small)
    {
        uint at;
        InterlockedAdd(counters[FX_COUNTER_LARGE], 1u, at);
        FX_RWBUFFER(uint, large, g_gridLarge);
        large[at] = id.x;
        return;
    }
    InterlockedAdd(counters[FX_COUNTER_GRID_ENTRIES], cells);
    for (uint k = 0u; k < cells; ++k) InterlockedAdd(counts[fxGridHash(fxGridCellOf(a, span, k)) & g_gridMask], 1u);
}
