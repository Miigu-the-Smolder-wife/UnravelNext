// unx-kernel: cs_6_6 main
// Surface state test probe (SurfaceTests.cpp): surfaceStateAt at a list of world points.
// P[0] = { points SRV (float4: x, y, z, 0), results UAV (float4 x 2 per point: wet, scorch, frost, dust; blood, snow, 0, 0),
//          count, 0 }, P[1] = { constants, table, pool } (raw SRVs).
#include "Passes/Decal/SurfaceState.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    StructuredBuffer<float4> points = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].y];
    SurfaceContext c;
    c.constants = P[1].x;
    c.table = P[1].y;
    c.pool = P[1].z;
    const SurfaceSample s = surfaceStateAt(c, points[id.x].xyz);
    results[2 * id.x] = float4(s.wet, s.scorch, s.frost, s.dust);
    results[2 * id.x + 1] = float4(s.blood, s.snow, 0, 0);
}
