// unx-kernel: cs_6_6 main
// The atmosphere record row alone (the transmittance LUT's row size.y + 1, as Transmittance.hlsl writes it) when only the
// record's per-view words changed (B5: the cloud layer's SRVs), without rebuilding the LUTs.
// P[0].x params (raw buffer, AtmosphereParams), P[0].y the transmittance LUT (UAV)
#include "Bindless.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

[numthreads(16, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= 11) return;
    const AtmosphereParams a = airLoadParams(P[0].x);
    ByteAddressBuffer raw = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> record = ResourceDescriptorHeap[P[0].y];
    record[uint2(id, a.transmittanceSize.y + 1)] = asfloat(raw.Load4(id * 16));
}
