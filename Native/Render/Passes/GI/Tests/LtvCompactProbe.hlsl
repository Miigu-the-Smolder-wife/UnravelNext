// unx-kernel: lib_6_6 main
// unx-variants: MODE=0,1
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"
#include "Passes/GI/LumenTranslucencyVolumeCompact.hlsli"
[shader("raygeneration")]
void LtvCompactTestGen()
{
    uint3 id, cell;
    float3 origin;
#if MODE == 0
    id = DispatchRaysIndex();
    cell = uint3(id.xy / LTV_TRACE_RES, id.z);
    if (!ltvCellVisible(cell, P[0].z)) return;
    float3 offset = ltvFrameJitter();
    ltvDepthConstraint(cell, offset, P[0].y, asfloat(P[9].z));
    origin = ltvCellPosition(float3(cell) + offset);
#else
    ltvCompactRay(P[10].x, DispatchRaysIndex().x + P[4].w, id, cell, origin);
#endif
    const uint3 grid = ltvGridSize();
    const uint index = id.x + grid.x * LTV_TRACE_RES * (id.y + grid.y * LTV_TRACE_RES * id.z);
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].x];
    output.Store4(index * 16, uint4(asuint(origin), index + 1));
}
