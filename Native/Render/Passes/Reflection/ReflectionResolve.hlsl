// unx-kernel: cs_6_6 main
// Reflection resolve (ARCHITECTURE 2.6: "SR = evaluation"), one group per tile that ReflectionClassify marked. Per pixel:
//   K: a = 0 (M evaluates it);
//   M: its own job's result;
//   G: the jobs at the corners of its cell on its spacing's grid (multiples of s; ReflectionJobs makes each G corner a
//      job), weighted by bilinear position, distance to the pixel's tangent plane and normal agreement; when none agrees
//      (an object edge) a = 0 this frame (K fallback) and the history's distance is stored negative: next frame the pixel
//      has its own job (REFL_SELF, ReflectionClassify). An own-job pixel takes its result and keeps the negative flag while
//      its grid still would not serve it.
// Also stores the reflection hit distance for next frame's G spacing and the value's hit motion (history, RG16F:
// distance (sign: the edge flag above), largest reflResultMotion of the samples used; ReflectionAccumulate limits the time
// window by it).
// Planar mirror pixels read their reflection camera's colour at (pixel - rectangle origin), divided by the exposure.
// P[0] = { mode SRV, results SRV, depth SRV, gbuffer SRV }, P[1] = { reflection UAV, history UAV, rows H, planar SRV }
// P[2] = { width, height, planar byte offset, flags (bit 0: reflection.layer_mirror_lobe) }, P[3] = planar colour SRVs;
// frame constants b1 = main view.
// Reconstruction layers (reflection.layers; ReflectionInternal.hlsli): P[4] = { job layers SRV (raw; UNX_NONE: off),
// stochastic layer UAV, residual layer UAV, guide UAV }. Every pixel of the view gets its guide (mode 0: no layer value -
// K, planar, no data); an M or G pixel with a value also its demodulated stochastic part and residual, resolved from
// the jobs with the value's own weights (the G grid: the modulated share and the albedo are interpolated, then divided).
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/Reconstruct/LayerCommon.hlsli"

struct ResolveLayers
{
    float3 share;     // albedo x stochastic (modulated)
    float3 albedo;
    float3 residual;
    uint hitGuide;    // M: hit normal oct 8 + 8; G: log2 of the pixel's sample spacing (the filter's tap unit)
    float noData;     // the weight of the jobs without cache data at their hits (REFL_LAYER_NO_DATA)
};
ResolveLayers loadJobLayers(uint job)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[P[4].x];
    const uint4 a = b.Load4(job * REFL_LAYER_JOB_BYTES);
    const uint2 c = b.Load2(job * REFL_LAYER_JOB_BYTES + 16);
    ResolveLayers l;
    l.albedo = reflUnpackAlbedo(a.z);
    l.share = reflLayerRadiance(a.xy) * l.albedo;
    l.residual = reflLayerRadiance(c);
    l.hitGuide = a.y >> 16;
    l.noData = (a.w & REFL_LAYER_NO_DATA) ? 1.0 : 0.0;
    return l;
}
void storeLayers(uint2 pixel, ReflSurface s, float deviceDepth, uint mode, ResolveLayers l, float hitDistance)
{
    RWTexture2D<float4> layerS = ResourceDescriptorHeap[P[4].y];
    RWTexture2D<float4> layerG = ResourceDescriptorHeap[P[4].z];
    RWTexture2D<uint4> guide = ResourceDescriptorHeap[P[4].w];
    const uint albedo = reflPackAlbedo(l.albedo);
    layerS[pixel] = float4(min(l.share / reflUnpackAlbedo(albedo), 65504.0), 0);
    layerG[pixel] = float4(clamp(l.residual, -65504.0, 65504.0), 0);
    guide[pixel] = layerPackGuide(deviceDepth, s.normal, s.roughness, mode, l.noData >= 0.5, l.hitGuide, hitDistance, albedo);
}
void storeNoLayers(uint2 pixel)
{
    if (P[4].x == UNX_NONE) return;
    RWTexture2D<uint4> guide = ResourceDescriptorHeap[P[4].w];
    guide[pixel] = uint4(0, 0, 0, 0);
}

// The distance the next classification reads: an exponential mean (weight 1/4) of this pixel's hit distances, so its G
// spacing (blur / 3) follows the scene and not one frame's 4-ray mean: noisy spacing wishes put neighbouring pixels on
// different levels, and every level wanted near a grid point costs a job there (ReflectionJobs) [measured: city 4K +1.4 ms
// with last-frame distances].
float reflSmoothedDistance(float previous, float d) { return previous != 0 ? lerp(abs(previous), d, 0.25) : d; }

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[1].x];
    const uint rows = P[1].z;
    const uint2 size = P[2].xy;
    const uint2 pixel = tile * 8 + local;
    if (any(pixel >= size)) return;
    if (reflection[uint2(tile.x, rows + tile.y)].a < 0.5)  // untouched tile: all K
    {
        storeNoLayers(pixel);
        return;
    }
    const bool layers = P[4].x != UNX_NONE;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float2> history = ResourceDescriptorHeap[P[1].y];
    const uint m = modes.Load(int3(pixel, 0));
    const uint mode = reflMode(m);
    if (mode == REFL_K)
    {
        reflection[pixel] = float4(0, 0, 0, 0);
        storeNoLayers(pixel);
        return;
    }
    if (mode == REFL_PLANAR)
    {
        const uint k = (m >> 2) & 7u;
        const ReflPlanar pl = reflPlanar(P[1].w, P[2].z, k);
        Texture2D<float4> colour = ResourceDescriptorHeap[P[3][k]];
        reflection[pixel] = float4(reflStorable(colour.Load(int3(pixel - pl.rect.xy, 0)).rgb / g_exposure), 1);
        storeNoLayers(pixel);
        return;
    }
    const float deviceDepth = layers ? depth.Load(int3(pixel, 0)) : 0;
    if (mode == REFL_M)
    {
        const uint3 r = results[reflJob(m)];
        reflection[pixel] = float4(reflStorable(reflResultRadiance(r)), 1);
        history[pixel] = float2(reflSmoothedDistance(history[pixel].x, reflResultDistance(r)), reflResultMotion(r));
        if (layers)
        {
            ResolveLayers own = loadJobLayers(reflJob(m));
            // reflection.layer_mirror_lobe: the value without the stochastic share is a layer too (LayerDenoise)
            if (P[2].w & 1u) own.residual = reflResultRadiance(r) - own.share;
            storeLayers(pixel, reflSurface(depth, gbuffer, pixel), deviceDepth, LAYER_MODE_M, own, reflResultDistance(r));
        }
        return;
    }
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    // G: bilinear over its spacing's grid (sample positions i s), in tiles R marked this frame.
    const int sp = (int)reflSpacing(m);
    const float2 f = float2(pixel) / sp;
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    float3 sum = 0;
    float dist = 0, weight = 0;
    float motion = 0;
    ResolveLayers sumLayers = (ResolveLayers)0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = (i0 + o) * sp;
        if (any(q < 0) || any(q >= int2(size))) continue;
        if (reflection[uint2(q.x / 8, rows + q.y / 8)].a < 0.5) continue;  // tile not classified this frame
        const uint mq = modes.Load(int3(q, 0));
        if (reflMode(mq) != REFL_G || reflJob(mq) == REFL_NO_JOB || (all(q == int2(pixel)) && (m & REFL_SELF) != 0)) continue;  // an own-job pixel tests its grid without itself
        // Result and surface are independent reads; issue both before the
        // tangent-plane/lobe weight. Accumulation remains k = 0..3.
        const uint3 r = results[reflJob(mq)];
        const ReflSurface t = reflSurface(depth, gbuffer, uint2(q));
        const float plane = abs(dot(s.normal, t.position - s.position)) / max(s.linearDepth, 1e-4);
        const float w = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * pow(saturate(1 - plane / 0.02), 2) * pow(saturate(dot(s.normal, t.normal)), 8) *
                        saturate(1 - abs(s.roughness - t.roughness) * 4);
        if (w <= 0) continue;
        sum += w * reflResultRadiance(r);
        dist += w * reflResultDistance(r);
        weight += w;
        motion = max(motion, reflResultMotion(r));
        if (layers)
        {
            const ResolveLayers l = loadJobLayers(reflJob(mq));
            sumLayers.share += w * l.share;
            sumLayers.albedo += w * l.albedo;
            sumLayers.residual += w * l.residual;
            sumLayers.noData += w * l.noData;
        }
    }
    // Reflected edges inside a cell (a nearby wall against far content: -16.6 % near a furnace wall [measured]) are handled
    // by the spacing: a job's distance is its lobe's nearest hit (ReflectionCombine), so near content sets the blur and the
    // grid is fine where the reflection is sharp. (A per-pixel rule "corner distances within 2x, else an own job" compared
    // 4-ray means that one sky ray moves by orders of magnitude: 1.24 M G samples and r.refl.shade 17 ms at city 4K
    // against 0.31 M [measured, 2026-09-27].)
    const bool gridServes = weight >= 1e-4;
    if ((m & REFL_SELF) != 0)
    {
        // Its own job; the flag stays while the grid would still not serve it.
        const uint3 r = results[reflJob(m)];
        const float d = max(reflSmoothedDistance(history[pixel].x, reflResultDistance(r)), 1e-3);
        reflection[pixel] = float4(reflStorable(reflResultRadiance(r)), 1);
        history[pixel] = float2(gridServes ? d : -d, reflResultMotion(r));
        if (layers)
        {
            ResolveLayers own = loadJobLayers(reflJob(m));
            own.hitGuide = 0;  // its own job: every pixel around it is a sample or interpolates finer ones
            storeLayers(pixel, s, deviceDepth, LAYER_MODE_G, own, reflResultDistance(r));
        }
        return;
    }
    if (!gridServes)
    {
        reflection[pixel] = float4(0, 0, 0, 0);
        history[pixel] = float2(-max(abs(history[pixel].x), 1e-3), 0);  // own job next frame
        storeNoLayers(pixel);
        return;
    }
    reflection[pixel] = float4(reflStorable(sum / weight), 1);
    history[pixel] = float2(reflSmoothedDistance(history[pixel].x, dist / weight), motion);
    if (layers)
    {
        sumLayers.share /= weight;
        sumLayers.albedo /= weight;
        sumLayers.residual /= weight;
        sumLayers.noData /= weight;
        sumLayers.hitGuide = (m >> 2) & 7u;
        storeLayers(pixel, s, deviceDepth, LAYER_MODE_G, sumLayers, dist / weight);
    }
}
