// unx-kernel: cs_6_6 main
// r.gi.sao.temporal (LumenShortRangeAO.hlsli): full resolution, one thread per pixel.
//   upsample  one of the 4 half-resolution pixels around the pixel, drawn with probability proportional to its
//             triangle-filter weight x plane weight x normal weight (the mean over frames is the edge-aware bilinear);
//   clamp     the history to mean +- scale x standard deviation of the pick's 3 x 3 half-resolution neighbourhood;
//   history   the surface point's previous position (the vis buffer's triangle in its previous vertices), bilinear over
//             the previous pixels that hold the same surface (stored view depth within the threshold of the point's);
//             value = lerp(history, now, 1 / (1 + n)), n = min(n_prev + 1, max frames).
// Output RGBA16F: xyz = world bent normal x AO, a = accumulated frames + 1 (0: no surface). Also this frame's view depth
// (the next frame's key).
// P[0] = { half-resolution AO (R32_UINT), depth, G-buffer, 0 }; sky = device depth 0
// P[1] = { width, height, factor | flags << 8 (1: history valid, 2: temporal on), clamp scale (float) }
// P[2] = { previous output, previous view depth, vis id, visible clusters } (UNX_NONE: none)
// P[3] = { output UAV, view depth UAV, max frames (float), history distance threshold (float) }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"
#include "Passes/GI/LumenShortRangeAO.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    const uint2 size = P[1].xy;
    if (any(pixel >= size)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[3].x];
    RWTexture2D<float> outDepth = ResourceDescriptorHeap[P[3].y];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    if (!(depthTex[pixel] > 0))
    {
        output[pixel] = 0;
        outDepth[pixel] = 0;
        return;
    }
    Texture2D<uint> halfAo = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const uint factor = P[1].z & 0xFFu, flags = P[1].z >> 8;
    const float linearZ = linearDepth(depthTex[pixel]);
    outDepth[pixel] = linearZ;
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 worldPos = g_cameraPosition + D * linearZ;
    const float3 normal = octDecode(gbuffer[pixel].x);
    const float NoV = dot(normal, -normalize(D));
    const int2 halfSize = int2((size + factor - 1) / factor);

    // ---- the pick
    int2 pick = int2(pixel);
    if (factor >= 2)
    {
        const int2 base = int2(floor((float2(pixel) + 0.5) / factor - 0.5));
        float4 w = 0;
        for (uint i = 0; i < 4; ++i)
        {
            const int2 c = base + int2(i & 1, i >> 1);
            if (any(c < 0) || any(c >= halfSize)) continue;
            const uint2 at = lumenAoFullPixel(uint2(c), factor, g_frameIndex, size);
            if (!(depthTex[at] > 0)) continue;
            const float2 d = abs(float2(at) - float2(pixel));
            float3 Dn, Dnx, Dny;
            mPixelRay(float2(at) + 0.5, Dn, Dnx, Dny);
            const float plane = dot(g_cameraPosition + Dn * linearDepth(depthTex[at]) - worldPos, normal) / linearZ;
            const float angle = acos(saturate(dot(octDecode(gbuffer[at].x), normal)));
            const float nw = 1 - saturate(angle * (0.9 * 2 / 3.14159265));
            w[i] = max(2 - d.x, 0) * max(2 - d.y, 0) * exp2(-5000.0 * plane * plane) * nw * nw;
        }
        const float sum = w.x + w.y + w.z + w.w;
        uint choice = 0;
        if (sum > 1e-4)
        {
            const float u = lumenAoNoise(pixel, g_frameIndex, 2) * sum;
            choice = 3;
            if (u <= w.x && w.x > 0) choice = 0;
            else if (u <= w.x + w.y && w.y > 0) choice = 1;
            else if (u <= w.x + w.y + w.z && w.z > 0) choice = 2;
            if (!(w[choice] > 0)) choice = w.x > 0 ? 0 : (w.y > 0 ? 1 : 2);
        }
        pick = clamp(base + int2(choice & 1, choice >> 1), int2(0, 0), halfSize - 1);
    }
    const float3 now = lumenAoUnpack(halfAo[pick]);

    float3 value = now;
    float count = 0;
    if ((flags & 3u) == 3u)
    {
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[2].z, P[2].w, pixel, worldPos, normal, prevP, prevN, instance);
        float2 pp;
        float prevDepth;
        if (giPreviousPixel(prevP, float2(size), pp, prevDepth) && all(pp >= 0) && all(pp < float2(size)))
        {
            Texture2D<float4> previous = ResourceDescriptorHeap[P[2].x];
            Texture2D<float> prevKeys = ResourceDescriptorHeap[P[2].y];
            const float tolerance = prevDepth * asfloat(P[3].w) / lerp(0.1, 1.0, saturate(NoV));
            const float2 x = pp - 0.5;
            const int2 i0 = int2(floor(x));
            const float2 f = x - floor(x);
            float3 history = 0;
            float weight = 0, frames = 0;
            for (uint k = 0; k < 4; ++k)
            {
                const int2 c = i0 + int2(k & 1, k >> 1);
                if (any(c < 0) || any(c >= int2(size))) continue;
                const float d = prevKeys[c];
                const float4 h = previous[c];
                if (!(d > 0) || abs(d - prevDepth) >= tolerance || !(h.a > 0)) continue;
                const float bw = ((k & 1) ? f.x : 1 - f.x) * ((k >> 1) ? f.y : 1 - f.y);
                history += h.xyz * bw;
                frames += (h.a - 1) * bw;
                weight += bw;
            }
            if (weight > 0.01)
            {
                history /= weight;
                frames /= weight;
                const float scale = asfloat(P[1].w);
                if (scale > 0)
                {
                    float3 sum = now, sq = now * now;
                    float n = 1;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            if (ox == 0 && oy == 0) continue;
                            const int2 c = pick + int2(ox, oy);
                            if (any(c < 0) || any(c >= halfSize)) continue;
                            const float3 v = lumenAoUnpack(halfAo[c]);
                            sum += v;
                            sq += v * v;
                            n += 1;
                        }
                    const float3 mean = sum / n, sd = sqrt(max(sq / n - mean * mean, 0.0)) * scale;
                    history = clamp(history, mean - sd, mean + sd);
                }
                count = min(frames + 1, asfloat(P[3].z));
                value = lerp(history, now, 1 / (1 + count));
            }
        }
    }
    output[pixel] = float4(value, count + 1);
}
