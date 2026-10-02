// unx-kernel: cs_6_6 main
// r.gi.ltv.filter (LumenTranslucencyVolume.hlsli): one pass of the separable spatial filter over the cells' ray texels -
// each texel with the same texel of the P[0].w cells either side along the pass's axis, Gaussian weights of standard
// deviation P[9].y cells (Unreal: 3 samples a side, deviation 5 - close to a box). Only the cells the view sees into
// are averaged (ltvCellVisible: the others were not traced and hold 0 - the reference averages them in, which darkens
// the cells in front of every surface by the share of the filter that lies behind it).
// P[0] = { input SRV (Texture3D, grid xy * 3), output UAV, axis (0 x, 1 y, 2 z), samples a side }, P[1].x = depth pyramid SRV
// P[4] = { grid x, grid y, grid z, 0 }, P[9].y = asuint(standard deviation, cells)
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint3 cell = uint3(id.xy / LTV_TRACE_RES, id.z);
    if (any(cell >= ltvGridSize())) return;
    const uint2 texel = id.xy - cell.xy * LTV_TRACE_RES;
    Texture3D<float3> input = ResourceDescriptorHeap[P[0].x];
    RWTexture3D<float3> output = ResourceDescriptorHeap[P[0].y];
    const int3 axis = int3(P[0].z == 0 ? 1 : 0, P[0].z == 1 ? 1 : 0, P[0].z == 2 ? 1 : 0);
    const int reach = (int)P[0].w;
    const float deviation = max(asfloat(P[9].y), 1e-3);
    float3 sum = 0;
    float weights = 0;
    for (int o = -reach; o <= reach; ++o)
    {
        const int3 n = int3(cell) + axis * o;
        if (any(n < 0) || any(n >= int3(ltvGridSize()))) continue;
        if (o != 0 && !ltvCellVisible(uint3(n), P[1].x)) continue;
        const float w = exp(-(float)(o * o) / (2 * deviation * deviation));
        sum += w * input.Load(int4(n.xy * int(LTV_TRACE_RES) + int2(texel), n.z, 0));
        weights += w;
    }
    output[id] = sum / max(weights, 1e-5);
}
