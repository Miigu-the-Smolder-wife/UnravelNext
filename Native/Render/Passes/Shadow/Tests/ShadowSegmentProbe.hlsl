// unx-kernel: cs_6_6 main
// Test probe (VsmTests): vsmSegmentClassify (VsmSample.hlsli, fragment depth ranges of COVERAGE_REDESIGN 4.3) of listed
// segments and the sun visibility (vsmSunVisibility, flat receivers: normal = the level's sun axis, as the classifier's
// points) at 16 evenly spaced points of each, for the check lit -> all 1, umbra -> all 0.
// P[0].x segments SRV (StructuredBuffer<float4>, 4 per segment: p0 + footprint, p1, surface normal, surface point), P[0].y output UAV
// (RWStructuredBuffer<float>, 17 per segment: class, then the 16 visibilities), P[0].z count
// P[1] = page table, pool, blocks, search bound SRVs; P[2].x VSM constants CBV. Frame constants of the main view.
#include "Frame.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[P[0].y];
    VsmResources r;
    r.table = ResourceDescriptorHeap[P[1].x];
    r.pool = ResourceDescriptorHeap[P[1].y];
    r.blocks = ResourceDescriptorHeap[P[1].z];
    r.searchBound = ResourceDescriptorHeap[P[1].w];
    r.cbv = P[2].x;
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[2].x];
    const float4 a = segments[4 * i], b = segments[4 * i + 1], n = segments[4 * i + 2], sp = segments[4 * i + 3];
    const float tanSun = tan(g_sunAngularRadius);
    output[17 * i] = (float)vsmSegmentClassify(r, a.xyz, b.xyz, a.w, tanSun, sp.xyz, n.xyz);
    const float3 axis = vc.level[vsmLevelForFootprint(vc, a.w)].lightZ;
    [loop] for (uint j = 0; j < 16; ++j)
    {
        uint path;
        const float3 p = lerp(a.xyz, b.xyz, j / 15.0);
        output[17 * i + 1 + j] = vsmSunVisibility(r, p, axis, a.w, tanSun, vc.searchTaps, vc.filterTaps, path);
    }
}
