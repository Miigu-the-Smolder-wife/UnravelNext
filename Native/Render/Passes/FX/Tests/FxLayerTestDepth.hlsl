// unx-kernel: cs_6_6 main
// Test opaque depth for the particle render pass (ParticleLayerTests.cpp): a wall of device depth asfloat(P[0].z) over
// the pixels x < P[0].w, sky (0) elsewhere. P[0] = (0, depth UAV (R32_FLOAT), wall depth bits, wall width px).
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_viewWidth || id.y >= g_viewHeight) return;
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    depth[id.xy] = id.x < P[0].w ? asfloat(P[0].z) : 0.0f;
}
