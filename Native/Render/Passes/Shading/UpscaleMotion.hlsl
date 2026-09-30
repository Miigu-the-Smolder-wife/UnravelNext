// unx-kernel: cs_6_6 main
// unx-strict-fp
// Temporal upscale (Upscale.cpp), motion per internal sample: where the surface point a jittered sample sees was in the
// previous frame's unjittered view, as an output-UV offset: m = uv_now - uv_prev, uv_now the point's unjittered position
// (the sample centre k + 0.5 minus this frame's jitter), uv_prev its previous surface point (GiScreenHistory.hlsli
// giPreviousSurface: the vis buffer's triangle in its previous-tick vertices; still instances keep the point) projected
// with the previous unjittered view-projection. The sky: the pixel's direction under the previous view (rotation only).
// A point behind the previous camera: m = (2, 2) (the history lookup falls off the screen and is not used).
// RG32F (UV offsets need more than half precision at 4K: 1/3840 per pixel).
// P[0] = { vis id SRV (UNX_NONE: none), visible clusters SRV, depth SRV, motion UAV }, P[1] = { width, height,
// asuint(jitter x), asuint(jitter y) } (internal pixels), P[2..5] = previous minus current unjittered view-projection.
// Frame constants of the (jittered) main view.
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

float4 viewDeltaOf(float4 p)
{
    const float4x4 m = float4x4(asfloat(P[2]), asfloat(P[3]), asfloat(P[4]), asfloat(P[5]));
    return mul(m, p);
}

float4 currentUnjitteredClip(float4 p, float2 jitter, uint2 size)
{
    float4 c = mul(g_viewProj, p);
    c.xy -= float2(2 * jitter.x, -2 * jitter.y) / float2(size) * c.w;
    return c;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> motion = ResourceDescriptorHeap[P[0].w];
    const float2 jitter = asfloat(P[1].zw);
    const float d = depth.Load(int3(id, 0));
    float4 worldPoint;
    float3 displacement = 0;
    if (!(d > 0))
        worldPoint = float4(worldFromDepth(float2(id), 1e-6) - g_cameraPosition, 0);
    else
    {
        const float3 p = worldFromDepth(float2(id), d);
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[0].x, P[0].y, id, p, float3(0, 0, 1), prevP, prevN, instance);
        worldPoint = float4(p, 1);
        displacement = prevP - p;
    }
    const float4 current = currentUnjitteredClip(worldPoint, jitter, size);
    // previous VP * previous point - current VP * current point.
    // A still camera and surface give delta == 0 exactly, irrespective of depth
    // reconstruction error or the jitter phase. Do not invent subpixel motion.
    const float4 delta = viewDeltaOf(worldPoint + float4(displacement, 0)) +
                         currentUnjitteredClip(float4(displacement, 0), jitter, size);
    const float previousW = current.w + delta.w;
    float2 m = float2(2, 2);
    if (current.w > 1e-6 && previousW > 1e-6)
    {
        const float2 ndcDelta = (delta.xy - current.xy / current.w * delta.w) / previousW;
        m = ndcDelta * float2(-0.5, 0.5);
    }
    motion[id] = float4(all(isfinite(m)) ? m : float2(2, 2), d > 0 ? current.w : 0, d > 0 ? previousW : 0);
}
