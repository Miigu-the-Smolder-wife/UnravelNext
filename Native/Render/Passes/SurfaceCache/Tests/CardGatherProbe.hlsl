// unx-kernel: cs_6_6 main
// unx-variants: STAGE=0,1
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint size = P[0].w;
    if (any(id.xy >= size)) return;
#if STAGE == 0
    RWTexture2D<float3> light = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    const uint h = (id.x * 1664525u + id.y * 1013904223u) ^ P[1].x;
    light[id.xy] = float3(h & 65535u, (h >> 7) & 65535u, (h >> 16) & 65535u) / 32.0;
    depth[id.xy] = (h & 15u) == 0 ? 1.0 : float(h & 65535u) / 65535.0;
#else
    Texture2D<float3> light = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].z];
    const uint2 base = min(id.xy, size - 2u);
    const int3 at = int3(base, 0);
    const float2 f = float2((id.x * 13u + P[1].x) & 511u, (id.y * 19u + P[1].x) & 511u) / 512.0;
    const float4 weights = float4((1-f.x)*(1-f.y), f.x*(1-f.y), (1-f.x)*f.y, f.x*f.y);
    const float4 a = float4(depth.Load(at), depth.Load(at, int2(1,0)), depth.Load(at, int2(0,1)), depth.Load(at, int2(1,1)));
    const float4 b = clGatherDepth(depth, base, size);
    const float4 w = weights * select(a < 1.0, float4(1,1,1,1), float4(0,0,0,0));
    const float3 reference = w.x*light.Load(at) + w.y*light.Load(at, int2(1,0)) + w.z*light.Load(at, int2(0,1)) + w.w*light.Load(at, int2(1,1));
    const float4 v = weights * select(b < 1.0, float4(1,1,1,1), float4(0,0,0,0));
    const float3 gathered = v.x*light.Load(at) + v.y*light.Load(at, int2(1,0)) + v.z*light.Load(at, int2(0,1)) + v.w*light.Load(at, int2(1,1));
    result.Store((id.y * size + id.x) * 4, (any(asuint(a) != asuint(b)) ? 1u : 0u) | (any(asuint(reference) != asuint(gathered)) ? 2u : 0u));
#endif
}
