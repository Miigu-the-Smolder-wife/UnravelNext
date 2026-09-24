// unx-kernel: cs_6_6 main
// Reflection resolve (ARCHITECTURE 2.6: "SR = evaluation"), one group per tile that ReflectionClassify marked. Per pixel:
//   K: a = 0 (M evaluates it);
//   M: its own job's result;
//   G: the samples of its spacing's global grid around it (up to 4, in any marked tile), weighted by bilinear position,
//      distance to the pixel's tangent plane and normal agreement; a = 0 when none agrees (K fallback, counted).
// Also stores the reflection hit distance for next frame's G spacing (distance history, R16F).
// P[0] = { mode SRV, results SRV, depth SRV, gbuffer SRV }, P[1] = { reflection UAV, history UAV, rows H, 0 }
// P[2] = { width, height, 0, 0 }; frame constants b1 = main view.
#include "Passes/Reflection/ReflectionInternal.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[1].x];
    const uint rows = P[1].z;
    if (reflection[uint2(tile.x, rows + tile.y)].a < 0.5) return;  // untouched tile: all K
    const uint2 size = P[2].xy;
    const uint2 pixel = tile * 8 + local;
    if (any(pixel >= size)) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint2> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float> history = ResourceDescriptorHeap[P[1].y];
    const uint m = modes.Load(int3(pixel, 0));
    const uint mode = reflMode(m);
    if (mode == REFL_K)
    {
        reflection[pixel] = float4(0, 0, 0, 0);
        return;
    }
    if (mode == REFL_M)
    {
        const uint2 r = results[reflJob(m)];
        reflection[pixel] = float4(reflResultRadiance(r), 1);
        history[pixel] = reflResultDistance(r);
        return;
    }
    // G: bilinear over the spacing grid (sample positions s/2 + i s), in tiles R marked this frame.
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const int sp = (int)reflSpacing(m);
    const float2 f = (float2(pixel) - sp / 2) / sp;
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    float3 sum = 0;
    float dist = 0, weight = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = (i0 + o) * sp + sp / 2;
        if (any(q < 0) || any(q >= int2(size))) continue;
        if (reflection[uint2(q.x / 8, rows + q.y / 8)].a < 0.5) continue;  // tile not classified this frame
        const uint mq = modes.Load(int3(q, 0));
        if (reflMode(mq) != REFL_G || reflSpacing(mq) != (uint)sp || reflJob(mq) == REFL_NO_JOB) continue;
        const ReflSurface t = reflSurface(depth, gbuffer, uint2(q));
        const float plane = abs(dot(s.normal, t.position - s.position)) / max(s.linearDepth, 1e-4);
        const float w = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * pow(saturate(1 - plane / 0.02), 2) * pow(saturate(dot(s.normal, t.normal)), 8) *
                        saturate(1 - abs(s.roughness - t.roughness) * 4);
        if (w <= 0) continue;
        const uint2 r = results[reflJob(mq)];
        sum += w * reflResultRadiance(r);
        dist += w * reflResultDistance(r);
        weight += w;
    }
    if (weight < 1e-4)
    {
        reflection[pixel] = float4(0, 0, 0, 0);
        return;
    }
    reflection[pixel] = float4(sum / weight, 1);
    history[pixel] = dist / weight;
}
