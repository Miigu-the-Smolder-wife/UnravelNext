#ifndef UNX_LTV_COMPACT_HLSLI
#define UNX_LTV_COMPACT_HLSLI
// Header = visible cell count; each 16-byte record = original linear cell index
// and its full precision, depth-constrained world position. Order is immaterial:
// ray seeds, output texels and sample identities use the original cell coordinate.
void ltvCompactRay(uint listSrv, uint ray, out uint3 id, out uint3 cell, out float3 origin)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[listSrv];
    const uint4 entry = list.Load4(16 + (ray / 9u) * 16);
    const uint3 grid = ltvGridSize();
    cell = uint3(entry.x % grid.x, (entry.x / grid.x) % grid.y, entry.x / (grid.x * grid.y));
    const uint sample = ray % 9u;
    id = uint3(cell.xy * LTV_TRACE_RES + uint2(sample % LTV_TRACE_RES, sample / LTV_TRACE_RES), cell.z);
    origin = asfloat(entry.yzw);
}
#endif
