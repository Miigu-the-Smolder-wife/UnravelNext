// unx-kernel: cs_6_6 main
// S's wind cache (WindCache.hlsli): windAt at every cell centre of the 64^3 grid. P[0] = { header SRV (raw), cache UAV
// (RWTexture3D<float4>, RGBA16F), 0, 0 }. Dispatch cells / 4 per axis.
#include "Passes/Atmosphere/WindCache.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const WindHeader h = windHeader(P[0].x);
    if (any(id >= h.cells)) return;
    RWTexture3D<float4> cache = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<WindRecord> records = ResourceDescriptorHeap[h.recordsSrv];
    const float3 p = h.gridOrigin + (float3(id) + 0.5) * h.spacing;
    cache[id] = float4(windAt(records, h.recordCount, p - h.reference, h.time), 0);
}
