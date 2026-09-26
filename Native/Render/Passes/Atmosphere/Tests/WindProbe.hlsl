// unx-kernel: cs_6_6 main
// Test kernel (WindFieldTests): windAt (WindField.hlsli) at query points. P[0] = { records SRV (StructuredBuffer of
// WindRecord), record count, queries SRV (StructuredBuffer<float4>: xyz position, w time), output UAV }, P[1].x = count.
#include "Bindless.hlsli"
#include "Passes/Atmosphere/WindField.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[1].x) return;
    StructuredBuffer<WindRecord> records = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
    const float4 q = queries[id];
    output[id] = float4(windAt(records, P[0].y, q.xyz, q.w), 0);
}
