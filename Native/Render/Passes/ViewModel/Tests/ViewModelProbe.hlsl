// unx-kernel: cs_6_6 main
// View model test probe (ViewModelTests.cpp): viewModelClip of a clip position for instances 0 .. P[0].z - 1.
// P[0] = { results UAV (float4 per instance), 0, instance count, 0 }; frame constants b1 carry g_viewModelScale.
#include "Bindless.hlsli"
#include "Passes/ViewModel/ViewModel.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].x];
    results[id.x] = viewModelClip(loadInstance(id.x), float4(0.25f, -0.5f, 0.01f, 2.0f));
}
