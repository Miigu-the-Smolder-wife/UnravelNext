// unx-kernel: cs_6_6 main
// Reflection tests only: reflectionRadiance (the M-facing API) at every 4th pixel -> float4 (rgb, a), plus the pixel's
// path from the mode texture in w's integer part when a = 1 (1 = M, 2 = G); w = -1 where the pixel shows sky.
// P[0] = { reflection SRV, depth SRV, mode SRV, output UAV }, P[1] = { width, height, stride, 0 }
#include "Passes/Reflection/ReflectionInternal.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint stride = P[1].z;
    const uint2 count = P[1].xy / stride;
    if (any(id >= count)) return;
    const uint2 pixel = id * stride;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
    const uint index = id.y * count.x + id.x;
    if (depth.Load(int3(pixel, 0)) <= 0)
    {
        output[index] = float4(0, 0, 0, -1);
        return;
    }
    const float4 r = reflectionRadiance(P[0].x, pixel);
    output[index] = float4(r.rgb, r.a > 0.5 ? (float)reflMode(modes.Load(int3(pixel, 0))) : 0);
}
