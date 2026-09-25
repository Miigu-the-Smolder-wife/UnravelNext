// unx-kernel: cs_6_6 main
// Core test kernel: the material model tables through FrameConstants (g_specularAlbedoLut, g_materialModelLut) at a
// grid of (NoV, roughness): 5 floats per point (NoV, roughness, A, B, E) into a raw UAV.
//   P[0].x output UAV (raw), P[0].y points per axis
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Common/MaterialModel.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint n = P[0].y;
    if (i >= n * n) return;
    const float mu = ((i % n) + 0.37) / n, r = ((i / n) + 0.61) / n;
    const float2 ab = modelSpecularAlbedo(mu, r);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    o.Store4(20 * i, asuint(float4(mu, r, ab)));
    o.Store(20 * i + 16, asuint(modelDirectionalAlbedo(mu, r)));
}
