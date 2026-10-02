// unx-kernel: cs_6_6 main
// Temporal upscale (Upscale.cpp), motion per internal sample: where the surface point a jittered sample sees was in the
// previous frame's unjittered view, as an output-UV offset: m = uv_now - uv_prev, uv_now the point's unjittered position
// (the sample centre k + 0.5 minus this frame's jitter), uv_prev its previous surface point (GiScreenHistory.hlsli
// giPreviousSurface: the vis buffer's triangle in its previous-tick vertices; still instances keep the point) projected
// with the previous unjittered view-projection. The sky: the pixel's direction under the previous view (rotation only).
// A point behind the previous camera: m = (2, 2) (the history lookup falls off the screen and is not used).
// RG32F (UV offsets need more than half precision at 4K: 1/3840 per pixel).
// P[0] = { vis id SRV (UNX_NONE: none), visible clusters SRV, depth SRV, motion UAV }, P[1] = { width, height,
// asuint(jitter x), asuint(jitter y) } (internal pixels), P[2..5] = rows of the previous unjittered view-projection.
// P[6].x = previous depth UAV (R32F; UNX_NONE: none): the point's view depth in the previous frame (0: the sky, or
// behind the previous camera) - the temporal super resolution's parallax test (Tsr.hlsli).
// Frame constants of the (jittered) main view.
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

float4 prevClipOf(float4 p)
{
    const float4x4 m = float4x4(asfloat(P[2]), asfloat(P[3]), asfloat(P[4]), asfloat(P[5]));
    return mul(m, p);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float2> motion = ResourceDescriptorHeap[P[0].w];
    const float2 jitter = asfloat(P[1].zw);
    const float2 uvNow = (float2(id) + 0.5 - jitter) / float2(size);
    const float d = depth.Load(int3(id, 0));
    float4 prevClip;
    if (!(d > 0))
    {
        const float3 dir = worldFromDepth(float2(id), 1e-6) - g_cameraPosition;  // a point far along the pixel's ray
        prevClip = prevClipOf(float4(dir, 0));
    }
    else
    {
        const float3 p = worldFromDepth(float2(id), d);
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[0].x, P[0].y, id, p, float3(0, 0, 1), prevP, prevN, instance);
        prevClip = prevClipOf(float4(prevP, 1));
    }
    float2 m = float2(2, 2);
    if (prevClip.w > 1e-6)
    {
        const float2 ndc = prevClip.xy / prevClip.w;
        m = uvNow - float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    }
    motion[id] = all(isfinite(m)) ? m : float2(2, 2);
    if (P[6].x != UNX_NONE)
    {
        RWTexture2D<float> previousDepth = ResourceDescriptorHeap[P[6].x];
        previousDepth[id] = d > 0 && prevClip.w > 1e-6 && isfinite(prevClip.w) ? prevClip.w : 0.0;
    }
}
