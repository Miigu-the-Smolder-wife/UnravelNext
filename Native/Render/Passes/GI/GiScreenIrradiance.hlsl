// unx-kernel: cs_6_6 main
// r.gi.screen: M's per-pixel GI cache irradiance (front side) as a pass of its own, written to view.giIrradiance for M's
// shading kernel to read once (R_STATUS_KO.md 0, GI tile path verdict: the lookup costs 2.44 ms at 4K in a kernel of its
// own occupancy [measured], against ~3.4 ms inside M's shading kernel [expected]). The same function on the same inputs
// as M (GiScreenInputs.hlsli), so the only difference is the storage: RGBA16F, rgb = irradiance x the view's exposure
// (relative rounding <= 2^-11; pre-exposed so no value leaves the half range), a = 1 where the cache had data (weight
// > 0), 0 where M keeps the screen probes' irradiance. Pixels without a surface get 0.
// The cache's anchor visibility (gi.anchor_visibility, giScreenSeen) re-weights each level's corners where they are all
// converged. (A quad-shared form with batched corner loads, plus the visibility loads, measured 1.44 ms against this
// form's 0.77 without visibility at 1440p [measured, 20379fb, RTX 4080]: the shared form is removed.)
// P[0] = { cache SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { width, height, 0, 0 }; b1 = the main view.
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= P[1].xy)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    float3 worldPos, nv;
    float4 result = 0;
    if (giScreenInputs(pixel, depth.Load(int3(pixel, 0)), gbuffer.Load(int3(pixel, 0)), worldPos, nv))
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
        float weight;
        const float3 e = giCacheIrradianceScreen(cache, giHeader(cache), worldPos, nv, weight);
        result = weight > 0 ? float4(e * g_exposure, 1) : float4(0, 0, 0, 0);
    }
    output[pixel] = result;
}
