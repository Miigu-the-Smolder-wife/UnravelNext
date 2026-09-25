// unx-kernel: cs_6_6 main
// Collision surfaces of the tick in anchor space (NV_StreamSurface): thread per surface of the persistent table. A
// surface on a rigid body (body != NONE) is body-local: p = R(q) p_local + position with this tick's body frame, and its
// velocity field is the body's (velocity of the centre of mass, angular velocity about it); the other surfaces are copied.
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
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    StreamSurface s = table[id.x];
    if (s.body != FX_NONE)
    {
        FX_BUFFER(StreamBody, bodies, g_bodies);
        const StreamBody b = bodies[s.body];
        s.a = rotateQ(b.rotation, s.a) + b.position;
        s.b = rotateQ(b.rotation, s.b) + b.position;
        s.c = rotateQ(b.rotation, s.c) + b.position;
        s.velocity = b.velocity;
        s.angular = b.angular;
        s.origin = b.center;  // the surface velocity field turns about the centre of mass
    }
    surfaces[id.x] = s;
}
