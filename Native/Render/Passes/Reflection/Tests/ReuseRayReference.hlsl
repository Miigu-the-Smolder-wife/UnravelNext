// unx-kernel: cs_6_6 main
// Replay each source ray once for the spatial resolve's many neighbour reads.
// FP32 direction/pdf preserve the original estimator and random draws.
// P[0]: modes, depth, G-buffer SRVs, cache UAV; P[1]: width, height, frame, bias;
// P[2].x: material words SRV. One 16-byte row per classified job.
#include "Passes/Reflection/ReflectionReuse.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= P[1].xy)) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    const uint m = modes.Load(int3(pixel, 0));
    if (reflMode(m) != REFL_M || reflJob(m) == REFL_NO_JOB) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    g_reflWords = P[2].x;
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    float3 dir;
    float pdf;
    const bool valid = reuseRay(s, pixel, P[1].z, asfloat(P[1].w), dir, pdf);
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[0].w];
    cache.Store4(reflJob(m) * 16u, asuint(float4(dir, valid ? pdf : 0.0)));
}
