// unx-kernel: cs_6_6 main
// Evaluate the output-grid reflection field at this frame's internal sample rays.
// A bounded 6x6 reconstruction, with the same receiver identity and depth test.
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
float historyWeight(float x)
{
    x = abs(x);
    return x < 1e-4 ? 1 : x >= 3 ? 0 : 3 * sin(3.14159265 * x) * sin(3.14159265 * x / 3) / (9.8696044 * x * x);
}
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    const uint2 size = P[2].xy, outputSize = P[2].zw;
    if (any(p >= size)) return;
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    if (reflection[uint2(p.x / 8, size.y + p.y / 8)].a < 0.5) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[1].w];
    const uint mode = reflMode(modes.Load(int3(p, 0)));
    if (mode != REFL_M && mode != REFL_G) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    const uint id = vis.Load(int3(p, 0));
    if (id == VIS_NONE) return;
    const uint key = loadVisibleCluster(P[1].y, visVisibleCluster(id)).instance + 1;
    Texture2D<float4> control = ResourceDescriptorHeap[P[3].z];
    Texture2D<float4> history = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].z];
    const ReflSurface surface = reflSurface(depth, gbuffer, p);
    const float tolerance = surface.linearDepth * (1e-3 + 4 * g_tanHalfFovY / outputSize.y / max(abs(dot(surface.normal, surface.view)), 0.1));
    const float2 position = (float2(p) + 0.5 - asfloat(P[3].xy)) / float2(size) * float2(outputSize) - 0.5;
    const int2 first = int2(floor(position));
    const float2 fraction = frac(position);
    float3 sum = 0, lo = 3e38, hi = 0;
    float weight = 0;
    [unroll] for (int y = -2; y <= 3; ++y)
        [unroll] for (int x = -2; x <= 3; ++x)
        {
            const int2 q = first + int2(x, y);
            if (any(q < 0) || any(q >= int2(outputSize))) continue;
            const uint2 k = keys.Load(int3(q, 0));
            if (k.x != key || abs(asfloat(k.y) - surface.linearDepth) > tolerance) continue;
            const float3 c = history.Load(int3(q, 0)).rgb;
            const float w = historyWeight(x - fraction.x) * historyWeight(y - fraction.y);
            sum += w * c; weight += w; lo = min(lo, c); hi = max(hi, c);
        }
    if (weight > 0.5) reflection[p] = float4(reflStorable(max(control.Load(int3(p, 0)).rgb + clamp(sum / weight, lo, hi), 0)), reflection[p].a);
}
