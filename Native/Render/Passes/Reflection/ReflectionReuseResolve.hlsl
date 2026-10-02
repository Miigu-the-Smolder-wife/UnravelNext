// unx-kernel: cs_6_6 main
// Resolve of the ray-reuse reflection pipeline (ReflectionReuse.hlsli), one thread per pixel of the view: a traced
// pixel's value from its own ray and from the rays of 'samples' neighbours on a disk of radius
// lumen_reconstruction_radius x min(8 x roughness, 1) px (none at a roughness of 0.05 or less: a mirror keeps its ray).
// A neighbour's ray is taken as this pixel's sample: its hit point - at most as far as this pixel's own hit, which keeps
// contacts and stops the far background leaking in - seen from this pixel, weighted by this pixel's lobe D(h) over the
// density the ray was drawn with. The value is the weighted mean (a ratio estimate) in the filters' space.
// Output: rgb = the resolved radiance (nits), a = the nearest hit distance among the rays used (the image's depth behind
// the surface for the history), a < 0 where the pixel traced nothing.
// P[0] = { modes SRV, results SRV, depth SRV, gbuffer SRV }, P[1] = { reflection SRV (tile rows), resolved UAV, rows H, frame }
// P[2] = { width, height, samples, flags (bit 0: no reconstruction - each pixel's own ray) }
// P[3] = { asuint(radius px), asuint(ray intensity cap), asuint(tone-map range), asuint(GGX sampling bias) }; frame constants b1 = main view.
#include "Passes/Reflection/ReflectionReuse.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    const uint2 size = P[2].xy;
    const uint2 pixel = tile * 8 + local;
    if (any(pixel >= size)) return;
    RWTexture2D<float4> resolved = ResourceDescriptorHeap[P[1].y];
    Texture2D<float4> reflection = ResourceDescriptorHeap[P[1].x];
    if (reflection.Load(int3(tile.x, P[1].z + tile.y, 0)).a < 0.5)  // a tile without traced or planar pixels
    {
        resolved[pixel] = float4(0, 0, 0, -1);
        return;
    }
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    const uint m = modes.Load(int3(pixel, 0));
    if (reflMode(m) != REFL_M)
    {
        resolved[pixel] = float4(0, 0, 0, -1);
        return;
    }
    StructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const float cap = asfloat(P[3].y), range = asfloat(P[3].z);
    const uint frame = P[1].w;
    g_reflWords = P[4].x;  // (M's material word: the top layer's roughness, ReflectionInternal.hlsli; UNX_NONE: none)
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const uint3 own = results[reflJob(m)];
    const float ownDistance = reflResultDistance(own);
    float3 sum = reuseToFilter(reuseCapIntensity(reflResultRadiance(own), cap), range);
    float weight = 1;
    float nearest = ownDistance;
    const float radius = s.roughness > 0.05 ? asfloat(P[3].x) * saturate(s.roughness * 8.0) : 0.0;
    const uint samples = P[2].z;
    if ((P[2].w & 1u) == 0 && radius > 1.0 && samples > 0)
    {
        const float alpha = max(s.roughness * s.roughness, 1e-4);
        const float a2 = alpha * alpha;
        // this pixel's own ray under the same weight (never dropped)
        float3 ownDir;
        float ownPdf;
        if (reuseRay(s, pixel, frame, asfloat(P[3].w), ownDir, ownPdf))
        {
            weight = max(reuseGgxD(a2, saturate(dot(s.normal, normalize(s.view + ownDir)))) / ownPdf, 1e-3);
            sum *= weight;
        }
        const uint key = reuseHash(pixel.x + pixel.y * 65536u + (frame & 7u) * 0x9E3779B9u);
        const float2 shift = float2(reuseUnit(key), reuseUnit(key + 1));
        [loop] for (uint i = 0; i < samples; ++i)
        {
            const int2 q = int2(floor(float2(pixel) + 0.5 + reuseDisk(i, samples, shift) * radius));
            if (any(q < 0) || any(q >= int2(size)) || all(q == int2(pixel))) continue;
            const uint mq = modes.Load(int3(q, 0));
            if (reflMode(mq) != REFL_M) continue;
            const ReflSurface t = reflSurface(depth, gbuffer, uint2(q));
            if (!t.valid) continue;
            float3 dir;
            float pdf;
            if (!reuseRay(t, uint2(q), frame, asfloat(P[3].w), dir, pdf)) continue;
            const uint3 r = results[reflJob(mq)];
            const float d = min(reflResultDistance(r), ownDistance);
            const float3 toHit = t.position + dir * d - s.position;
            const float reach = length(toHit);
            const float3 seen = reach > 0 ? toHit / reach : dir;
            if (dot(seen, s.normal) <= 0) continue;  // (under this pixel's horizon: not a direction of its lobe)
            const float w = reuseGgxD(a2, saturate(dot(s.normal, normalize(s.view + seen)))) / pdf;
            if (!(w > 1e-6)) continue;
            sum += reuseToFilter(reuseCapIntensity(reflResultRadiance(r), cap), range) * w;
            weight += w;
            nearest = min(nearest, d);
        }
    }
    resolved[pixel] = float4(reflStorable(reuseFromFilter(sum / weight, range)), nearest);
}
