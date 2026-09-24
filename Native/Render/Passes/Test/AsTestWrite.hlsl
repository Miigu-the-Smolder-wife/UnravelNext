// unx-kernel: cs_6_6 main
// Render-graph acceleration-structure test: writes one triangle (z = 5, facing -z) and one identity instance descriptor
// that points at the BLAS, so the following BLAS/TLAS builds consume GPU-written inputs.
// P[0].x vertex buffer UAV (raw, 3 x float3), P[0].y instance descriptor UAV (raw, 64 B), P[0].zw BLAS address lo/hi
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer instance = ResourceDescriptorHeap[P[0].y];
    vertices.Store3(0, asuint(float3(-1, -1, 5)));
    vertices.Store3(12, asuint(float3(0, 1, 5)));
    vertices.Store3(24, asuint(float3(1, -1, 5)));
    // D3D12_RAYTRACING_INSTANCE_DESC: 3x4 transform, InstanceID:24 | Mask:8, HitGroupIndex:24 | Flags:8, BLAS address.
    instance.Store4(0, asuint(float4(1, 0, 0, 0)));
    instance.Store4(16, asuint(float4(0, 1, 0, 0)));
    instance.Store4(32, asuint(float4(0, 0, 1, 0)));
    instance.Store4(48, uint4(0xFFu << 24, 0, P[0].z, P[0].w));
}
