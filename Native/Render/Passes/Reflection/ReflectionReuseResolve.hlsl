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
// P[4].z = reflection.lumen_downsample: factor (1 or 2) | this frame's offset x << 8 | y << 16 (ReflectionClassify). With
// factor 2 a block of 2 x 2 pixels has one ray: a neighbour is the traced pixel of the block the sample falls in, and a
// pixel without its own ray first takes the rays of its block and of the three blocks on its side (the bilinear set),
// whatever its roughness.
#include "Passes/Reflection/ReflectionReuse.hlsli"
#ifndef REUSE_CACHED_RAYS
#define REUSE_CACHED_RAYS 0
#endif

// The traced pixel of q's block (q itself at factor 1) and its mode word; false: the block traced nothing.
bool blockRay(Texture2D<uint> modes, int2 q, int2 size, out int2 traced, out uint word)
{
    traced = q;
    word = 0;
    const uint factor = P[4].z & 0xFFu;
    if (factor <= 1)
    {
        word = modes.Load(int3(q, 0));
        return reflMode(word) == REFL_M && reflJob(word) != REFL_NO_JOB;
    }
    const int2 block = q & ~1;
    const int2 offset = int2((P[4].z >> 8) & 1u, (P[4].z >> 16) & 1u);
    [unroll] for (int k = -1; k < 4; ++k)
    {
        const int2 p = block + (k < 0 ? offset : int2(k & 1, k >> 1));
        if (any(p >= size)) continue;
        word = modes.Load(int3(p, 0));
        if (reflMode(word) == REFL_M && reflJob(word) != REFL_NO_JOB)
        {
            traced = p;
            return true;
        }
    }
    return false;
}

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
#if REUSE_CACHED_RAYS
    ByteAddressBuffer rayCache = ResourceDescriptorHeap[P[4].w];
#endif
    const float cap = asfloat(P[3].y), range = asfloat(P[3].z);
    const uint frame = P[1].w;
    g_reflWords = P[4].x;  // (M's material word: the top layer's roughness, ReflectionInternal.hlsli; UNX_NONE: none)
    reuseSnapExposure(P[4].y);  // (the snap frame's exposure reference, ReflectionReuse.hlsli; UNX_NONE: none)
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const bool hasOwn = reflJob(m) != REFL_NO_JOB;
    float ownDistance = 65000.0;
    float3 sum = 0;
    float weight = 0;
    float nearest = 65000.0;
    if (hasOwn)
    {
        const uint3 own = results[reflJob(m)];
        ownDistance = reflResultDistance(own);
        sum = reuseToFilter(reuseCapIntensity(reflResultRadiance(own), cap), range);
        weight = 1;
        nearest = ownDistance;
    }
    const float radius = s.roughness > 0.05 ? asfloat(P[3].x) * saturate(s.roughness * 8.0) : 0.0;
    const uint samples = P[2].z;
    const bool reconstruct = (P[2].w & 1u) == 0 && radius > 1.0 && samples > 0;
    // (a pixel without its own ray: a value without weights, should every weighted neighbour fall away)
    float3 fallback = 0;
    float fallbackDistance = 65000.0;
    bool hasFallback = false;
    if (reconstruct || !hasOwn)
    {
        const float alpha = max(s.roughness * s.roughness, 1e-4);
        const float a2 = alpha * alpha;
        // this pixel's own ray under the same weight (never dropped)
        float3 ownDir;
        float ownPdf;
#if REUSE_CACHED_RAYS
        const float4 ownRay = hasOwn ? asfloat(rayCache.Load4(reflJob(m) * 16u)) : float4(0, 0, 0, 0);
        ownDir = ownRay.xyz;
        ownPdf = ownRay.w;
        if (hasOwn && ownPdf > 0)
#else
        if (hasOwn && reuseRay(s, pixel, frame, asfloat(P[3].w), ownDir, ownPdf))
#endif
        {
            weight = max(reuseGgxD(a2, saturate(dot(s.normal, normalize(s.view + ownDir)))) / ownPdf, 1e-3);
            sum *= weight;
        }
        const uint key = reuseHash(pixel.x + pixel.y * 65536u + (frame & 7u) * 0x9E3779B9u);
        const float2 shift = float2(reuseUnit(key), reuseUnit(key + 1));
        // (without an own ray: 4 block rays around the pixel first, then the disk)
        const uint fixedSamples = hasOwn ? 0u : 4u;
        const uint diskSamples = reconstruct ? samples : 0u;
        const int2 side = int2((pixel.x & 1u) ? 2 : -2, (pixel.y & 1u) ? 2 : -2);
        [loop] for (uint i = 0; i < fixedSamples + diskSamples; ++i)
        {
            int2 q;
            if (i < fixedSamples) q = int2(pixel) + int2((i & 1u) ? side.x : 0, (i >> 1) ? side.y : 0);
            else q = int2(floor(float2(pixel) + 0.5 + reuseDisk(i - fixedSamples, diskSamples, shift) * radius));
            if (any(q < 0) || any(q >= int2(size))) continue;
            int2 traced;
            uint mq;
            if (!blockRay(modes, q, int2(size), traced, mq) || all(traced == int2(pixel))) continue;
            q = traced;
            float3 dir;
            float pdf;
#if REUSE_CACHED_RAYS
            const float d0 = depth.Load(int3(q, 0));
            const float4 ray = asfloat(rayCache.Load4(reflJob(mq) * 16u));
            if (!(d0 > 0) || !(ray.w > 0)) continue;
            const float3 position = worldFromDepth(float2(q), d0);
            dir = ray.xyz;
            pdf = ray.w;
#else
            const ReflSurface t = reflSurface(depth, gbuffer, uint2(q));
            if (!t.valid) continue;
            if (!reuseRay(t, uint2(q), frame, asfloat(P[3].w), dir, pdf)) continue;
            const float3 position = t.position;
#endif
            const uint3 r = results[reflJob(mq)];
            const float d = min(reflResultDistance(r), ownDistance);
            if (!hasFallback)
            {
                fallback = reuseToFilter(reuseCapIntensity(reflResultRadiance(r), cap), range);
                fallbackDistance = d;
                hasFallback = true;
            }
            const float3 toHit = position + dir * d - s.position;
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
    if (!(weight > 0))
    {
        if (!hasFallback)
        {
            resolved[pixel] = float4(0, 0, 0, -1);
            return;
        }
        sum = fallback;
        weight = 1;
        nearest = fallbackDistance;
    }
    resolved[pixel] = float4(reflStorable(reuseFromFilter(sum / weight, range)), nearest);
}
