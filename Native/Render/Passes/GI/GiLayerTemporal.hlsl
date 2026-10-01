// unx-kernel: cs_6_6 main
// L_gi's temporal step (redesign V2 1.2, P2; gi.screen_temporal_frames): after r.gi.screen.filter has reconstructed the
// pixel's front indirect irradiance in space this frame, a short history (at most N frames) averages what that left.
// The history of a pixel is its own surface point's value of the previous frame: the point's exact previous position
// (GiScreenHistory.hlsli giPreviousSurface: the vis buffer's triangle in its previous-tick vertices, so moving objects keep
// theirs) in the previous view, bilinear over the 4 previous pixels around it; each of the 4 must hold the same surface
// (its stored depth unprojected within 2 pixel footprints of the point and near its plane, its normal in the same cache
// normal class and within 8 degrees, with data), else the pixel starts anew (disocclusion, edges, the first frame) - the
// spatial reconstruction alone is then the value (the first-frame rule of 1.0). The history is clipped to the mean +- 3
// standard deviations of this frame's 3 x 3 neighbourhood (a light that changed, an object that moved past: no ghost of
// the old value), rescaled by this frame's exposure over the previous one, and blended as a running mean of at most N
// frames: E = (E_hist (n - 1) + E_now) / n, n = min(n_prev + 1, N).
// Output RGBA16F: rgb = E x exposure (as r.gi.screen.filter's), a = n (>= 1 where there is data: M's a > 0 test), 0
// without data. Keys per pixel for the next frame: device depth, octahedral normal (15 + 15 bits); 0 = no surface.
// P[0] = { filtered SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { width, height, flags (bit 0: the previous frame's
// history is valid), exposure ratio (float) }, P[2] = { previous keys SRV, keys UAV, previous value SRV, vis id SRV },
// P[3] = { visible clusters SRV, N, 0, 0 }, P[4..7] = the previous frame's inverse view-projection (rows). b1 = the view.
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

uint giLayerPackNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : octWrap(n.xy);
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 16383.0));
    return (uint(q.x) & 0x7FFFu) | ((uint(q.y) & 0x7FFFu) << 15);
}
float3 giLayerUnpackNormal(uint packed)
{
    const float2 e = float2(int2(packed << 17, packed << 2) >> 17) / 16383.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = octWrap(n.xy);
    return normalize(n);
}

// The previous frame's value at this pixel's surface point (rgb x the previous exposure, a = its n); false when any of the
// 4 previous pixels holds another surface or no data.
bool giLayerHistory(uint2 pixel, float3 worldPos, float3 nv, float footprint, out float4 value)
{
    value = 0;
    float3 prevP, prevN;
    uint instance;
    giPreviousSurface(P[2].w, P[3].x, pixel, worldPos, nv, prevP, prevN, instance);
    if (instance == 0) return false;  // no vis id: no exact motion
    const float2 size = float2(P[1].xy);
    float2 prevPixel;
    float prevDepth;
    if (!giPreviousPixel(prevP, size, prevPixel, prevDepth)) return false;
    const float2 x = prevPixel - 0.5;
    const int2 i0 = int2(floor(x));
    if (any(i0 < 0) || any(i0 + 1 >= int2(P[1].xy))) return false;
    const float2 f = x - floor(x);
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[2].x];
    Texture2D<float4> values = ResourceDescriptorHeap[P[2].z];
    const float4x4 invPrev = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
    const uint nc = giNormalClass(nv);
    const float reach = 2 * footprint / max(abs(dot(nv, normalize(g_cameraPosition - worldPos))), 0.25);
    float4 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 q = i0 + int2(k & 1, k >> 1);
        const uint2 key = keys.Load(int3(q, 0));
        if (key.x == 0) return false;
        const float2 ndc = float2((q.x + 0.5) / size.x * 2 - 1, 1 - (q.y + 0.5) / size.y * 2);
        const float4 hp = mul(invPrev, float4(ndc, asfloat(key.x), 1));
        const float3 s = hp.xyz / hp.w;
        const float d = distance(s, prevP);
        if (!(d <= reach) || !(abs(dot(prevN, s - prevP)) <= footprint + 0.5 * d)) return false;
        const float3 n = giLayerUnpackNormal(key.y);
        if (giNormalClass(n) != nc || dot(n, prevN) < 0.99) return false;
        const float4 v = values.Load(int3(q, 0));
        if (!(v.a > 0)) return false;
        const float w = (k & 1 ? f.x : 1 - f.x) * (k & 2 ? f.y : 1 - f.y);
        sum += w * v;
    }
    value = sum;
    return true;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(pixel >= size)) return;
    Texture2D<float4> filtered = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<uint2> keysOut = ResourceDescriptorHeap[P[2].y];
    const float depthValue = depth.Load(int3(pixel, 0));
    float3 worldPos, nv;
    const bool surface = giScreenInputs(pixel, depthValue, gbuffer.Load(int3(pixel, 0)), worldPos, nv);
    keysOut[pixel] = surface ? uint2(asuint(depthValue), giLayerPackNormal(nv)) : uint2(0, 0);
    const float4 now = filtered.Load(int3(pixel, 0));
    if (!surface || !(now.a > 0))
    {
        output[pixel] = float4(now.rgb, 0);
        return;
    }
    float4 history;
    const float footprint = 2 * linearDepth(depthValue) * g_tanHalfFovY / g_viewHeight;
    if ((P[1].z & 1u) == 0 || !giLayerHistory(pixel, worldPos, nv, footprint, history))
    {
        output[pixel] = float4(now.rgb, 1);
        return;
    }
    // this frame's 3 x 3 neighbourhood (pixels with data) bounds the history: mean +- 3 sigma per channel
    float3 m1 = 0, m2 = 0;
    float count = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(size) - 1);
            const float4 v = filtered.Load(int3(q, 0));
            if (!(v.a > 0)) continue;
            m1 += v.rgb;
            m2 += v.rgb * v.rgb;
            count += 1;
        }
    const float3 mean = m1 / count, sd = sqrt(max(m2 / count - mean * mean, 0.0));
    const float3 hist = clamp(history.rgb * asfloat(P[1].w), mean - 3 * sd, mean + 3 * sd);
    const float n = min(floor(history.a + 0.5) + 1, (float)P[3].y);
    output[pixel] = float4((hist * (n - 1) + now.rgb) / n, n);
}
