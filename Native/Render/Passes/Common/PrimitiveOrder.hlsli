// Stable geometry identity. Visible-list slots and append-buffer elements are
// scheduling artifacts and must never break depth ties.
#ifndef UNX_PRIMITIVE_ORDER_HLSLI
#define UNX_PRIMITIVE_ORDER_HLSLI
#include "VisBuffer.hlsli"
uint64_t primitiveOrder(uint visibleSrv, uint visId)
{
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[visibleSrv];
    const uint2 v = visible[visVisibleCluster(visId)];
    return ((uint64_t)v.x << 31) | ((uint64_t)(v.y & 0xFFFFFFu) << 7) | visTriangle(visId);
}
#endif
