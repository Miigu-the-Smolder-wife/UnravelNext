// unx-kernel: cs_6_6 main
// Composition of the reflection layers back into view.reflection (LayerCommon.hlsli; ReflectionInternal.hlsli:
// value = base + residual + albedo x stochastic), one thread per pixel of the view. ReflectionResolve left the pixel's
// unfiltered value in view.reflection and its unfiltered layers beside it, so
//     value' = value + albedo x (stochastic' - stochastic) + (residual' - residual),
// with the primed layers the reconstructed ones (LayerDenoise, LayerTemporal): the base - an M pixel's reflected image
// without its stochastic light: its identity - never passes through a filter, and with the reconstruction off the
// value is the resolve's bit for bit. Pixels without layers (K, planar mirrors, no data) are left as they are.
// P[0] = { reflection UAV, guide SRV, stochastic SRV, residual SRV }, P[1] = { stochastic' SRV, residual' SRV, width, height }
// P[2] = { view (reflection.layer_view, diagnostics: 0 = the value; 1 stochastic, 2 stochastic', 3 residual, 4 residual',
// 5 albedo, 6 base, 7 = the reconstructed layers' alphas (frames in their history) - written instead of the value), 0, 0, 0 }
#include "Passes/Reconstruct/LayerCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[1].zw;
    if (any(pixel >= size)) return;
    Texture2D<uint4> guides = ResourceDescriptorHeap[P[0].y];
    const uint4 g = guides.Load(int3(pixel, 0));
    const uint mode = layerMode(g);
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    const uint view = P[2].x;
    if (mode == LAYER_MODE_NONE)
    {
        // (diagnostics: only pixels with layers show; a = 0 pixels are K anyway)
        if (view != 0 && reflection[pixel].a >= 0.5) reflection[pixel] = float4(0, 0, 0, 1);
        return;
    }
    const float4 total = reflection[pixel];
    if (total.a < 0.5) return;
    Texture2D<float4> rawS = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> rawR = ResourceDescriptorHeap[P[0].w];
    Texture2D<float4> newS = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> newR = ResourceDescriptorHeap[P[1].y];
    const float3 albedo = reflUnpackAlbedo(g.w);
    const float3 s0 = rawS.Load(int3(pixel, 0)).rgb;
    const float4 s1 = newS.Load(int3(pixel, 0));
    float3 r0 = 0;
    float4 r1 = 0;
    if (mode == LAYER_MODE_G)
    {
        r0 = rawR.Load(int3(pixel, 0)).rgb;
        r1 = newR.Load(int3(pixel, 0));
    }
    float3 value = max(total.rgb + albedo * (s1.rgb - s0) + (r1.rgb - r0), 0.0);
    if (any(isnan(value)) || any(isinf(value))) value = total.rgb;  // (never expected: the resolve's value stays)
    if (view == 1) value = s0;
    else if (view == 2) value = s1.rgb;
    else if (view == 3) value = abs(r0);
    else if (view == 4) value = abs(r1.rgb);
    else if (view == 5) value = albedo;
    else if (view == 6) value = max(total.rgb - albedo * s0 - r0, 0.0);
    else if (view == 7) value = float3(s1.a, r1.a, 0);
    reflection[pixel] = float4(reflStorable(value), 1);
}
