// unx-kernel: cs_6_6 main
// FroxelTests 7: synthetic particle media in E's volumeSlices layout (RGBA16F gridX x gridY x 2S: slice s's optical
// depth, then its self-attenuated source in nits), the formula FroxelTests.cpp mirrors (mediaAt).
// P[0] = { output UAV (RWTexture3D<float4>), gridX, gridY, slices }. One thread per (tile, slice).
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint gridX = P[0].y, gridY = P[0].z, S = P[0].w;
    const uint tile = id.x, s = id.y;
    if (tile >= gridX * gridY || s >= S) return;
    const uint tx = tile % gridX, ty = tile / gridX;
    float3 tau = 0, source = 0;
    if ((tx + ty) % 3 == 0 && s >= 6 && s < 18)
    {
        tau = 0.08 * (1 + 0.1 * (s - 6)) * float3(1, 0.85, 0.7);
        source = float3(900, 1200, 1500) * (1 + 0.03 * s);
    }
    if (ty % 4 == 1 && s >= 30 && s < 34)
    {
        tau += float3(0.5, 0.5, 0.5);
        source += float3(300, 250, 200);
    }
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].x];
    output[uint3(tx, ty, s)] = float4(tau, 0);
    output[uint3(tx, ty, S + s)] = float4(source, 0);
}
