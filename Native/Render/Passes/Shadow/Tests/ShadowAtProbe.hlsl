// unx-kernel: cs_6_6 main
// Test probe (VsmTests): shadowSunVisibilityAt (ShadowVisibility.hlsli, the ray-hit lookup) at listed world points, or
// shadowSunVisibilityInAir (points in the air, FX particles) where the normal's w is 1, or the air walk
// vsmAirShadowFraction (VsmAir.hlsli) of the segment position -> position + normal.xyz on level (uint) footprint where the
// normal's w is 2: output (shadowed fraction, 1 when the walk reached its hard cap; INTERFACES 3.6).
// P[0].x points SRV (StructuredBuffer<float4>: position, footprint m), P[0].y normals SRV (StructuredBuffer<float4>),
// P[0].z output UAV (RWStructuredBuffer<float2>: visibility, 1 = resident), P[0].w count
// P[1] = ShadowSrvs words 0..3 (page table, pool, blocks, search bound), P[2] = words 4..7 (constants, lights, slot of
// light, layers). Frame constants of the main view.
#include "Frame.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].w) return;
    StructuredBuffer<float4> points = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float4> normals = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float2> output = ResourceDescriptorHeap[P[0].z];
    ShadowSrvs s;
    s.pageTable = P[1].x;
    s.pool = P[1].y;
    s.blocks = P[1].z;
    s.searchBound = P[1].w;
    s.constants = P[2].x;
    s.lights = P[2].y;
    s.pad0 = P[2].z;
    s.layers = P[2].w;  // transmittance layer (FrameResources::vsmLayers)
    const float4 p = points[i];
    bool resident;
    const float4 n = normals[i];
    if (n.w == 2)
    {
        VsmResources r;
        r.table = ResourceDescriptorHeap[s.pageTable];
        r.pool = ResourceDescriptorHeap[s.pool];
        r.blocks = ResourceDescriptorHeap[s.blocks];
        r.searchBound = ResourceDescriptorHeap[s.searchBound];
        r.cbv = s.constants;
        VsmAirWalkCount count = (VsmAirWalkCount)0;
        const float f = vsmAirShadowFraction(r, p.xyz, p.xyz + n.xyz, (uint)p.w, count);
        output[i] = float2(f, count.capped ? 1.0 : 0.0);
        return;
    }
    const float v = n.w != 0 ? shadowSunVisibilityInAir(s, p.xyz, p.w, resident) : shadowSunVisibilityAt(s, p.xyz, n.xyz, p.w, resident);
    output[i] = float2(v, resident ? 1.0 : 0.0);
}
