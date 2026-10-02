// unx-kernel: cs_6_6 main
// Time accumulation of the ray-reuse reflection pipeline (ReflectionReuse.hlsli), one thread per pixel of the view.
// A traced pixel's resolved value joins a history of at most lumen_temporal_max_frames frames (2 for a mirror, rising to
// the maximum at a roughness of 0.05). Two places are tried for the history:
//   the image   where the reflected image was: the eye ray continued behind the surface for the resolved hit distance,
//               projected with the previous view - only for lobes narrow enough that the reflected direction decides
//               the value (dominant-direction factor above 0.5); taken wherever a history exists there;
//   the surface where the pixel's own surface point was a tick ago (GiScreenHistory.hlsli: exact motion), where the
//               previous frame's depth agrees (lumen_history_distance_threshold of the depth, wider at grazing angles
//               and dithered per pixel).
// Both are bounded by this frame's resolved neighbourhood - the mean +- lumen_neighborhood_clamp_scale standard
// deviations over the 21 pixels of the 5 x 5 block without its corners, per channel in YCoCg of the filters' space - and
// the one nearer the neighbourhood's mean is used (the surface history at 0.8 when the image history is the farther
// one). How far the history had to be pulled in lowers the frame count it carries on (confidence), so a change is
// followed in a few frames. Besides the value, the second moment of its luminance (filters' space) is accumulated: the
// temporal variance ReflectionReuseFilter steers by.
// P[0] = { resolved SRV, depth SRV, gbuffer SRV, vis id SRV (UNX_NONE: the pixel's own point) }
// P[1] = { visible clusters SRV, previous history SRV, previous frames SRV, previous keys SRV }
// P[2] = { history out UAV (rgb nits, a = second moment), frames out UAV, keys out UAV (x: 1 = a value, y: linear depth bits), flags
//          (bit 0: no history this frame) }
// P[3] = { width, height, asuint(max frames), asuint(clamp scale) }
// P[4] = { asuint(tone-map range), asuint(distance threshold), frame, M's material word SRV (the top layer's roughness,
// ReflectionInternal.hlsli g_reflWords; UNX_NONE: none) }; frame constants b1 = main view.
#include "Passes/Reflection/ReflectionReuse.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

struct ReuseHistory
{
    bool valid;
    float3 value;   // filters' space
    float moment, frames;
};

// The history's four pixels around a previous position; depthTest: only those whose depth is the surface's.
ReuseHistory reuseHistoryAt(float2 prevPixel, float2 size, bool depthTest, float expected, float tolerance, float range)
{
    ReuseHistory h;
    h.valid = false;
    h.value = 0;
    h.moment = h.frames = 0;
    if (any(prevPixel < 0) || any(prevPixel >= size)) return h;
    Texture2D<float4> prevValue = ResourceDescriptorHeap[P[1].y];
    Texture2D<float4> prevFrames = ResourceDescriptorHeap[P[1].z];
    Texture2D<uint2> prevKeys = ResourceDescriptorHeap[P[1].w];
    const float2 at = prevPixel - 0.5;
    const int2 i0 = int2(floor(at));
    const float2 f = at - floor(at);
    float weight = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 t = i0 + o;
        if (any(t < 0) || any(t >= int2(size))) continue;
        const uint2 key = prevKeys.Load(int3(t, 0));
        if (key.x == 0) continue;
        if (depthTest && abs(asfloat(key.y) - expected) >= tolerance) continue;
        const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        if (w <= 0) continue;
        const float4 v = prevValue.Load(int3(t, 0));
        h.value += w * reuseToFilter(v.rgb, range);
        h.moment += w * v.a;
        h.frames += w * prevFrames.Load(int3(t, 0)).r;
        weight += w;
    }
    if (weight > 0.01)
    {
        h.valid = true;
        h.value /= weight;
        h.moment /= weight;
        h.frames /= weight;
    }
    return h;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy;
    if (any(pixel >= size)) return;
    Texture2D<float4> resolved = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> valueOut = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<float4> framesOut = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint2> keysOut = ResourceDescriptorHeap[P[2].z];
    const float4 centre = resolved.Load(int3(pixel, 0));
    if (centre.a < 0)
    {
        valueOut[pixel] = float4(0, 0, 0, 0);
        framesOut[pixel] = float4(0, 0, 0, 0);
        keysOut[pixel] = uint2(0, 0);
        return;
    }
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const float range = asfloat(P[4].x);
    g_reflWords = P[4].w;
    reuseSnapExposure(P[5].x);  // (the snap frame's exposure reference, ReflectionReuse.hlsli; UNX_NONE: none)
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    float3 value = reuseToFilter(centre.rgb, range);
    float moment = reuseLuminance(value) * reuseLuminance(value);
    float frames = 0;
    const float maxFrames = reuseMaxFrames(asfloat(P[3].z), s.roughness);
    if ((P[2].w & 1u) == 0)
    {
        const float3 toEye = g_cameraPosition - s.position;
        const float eyeDistance = length(toEye);
        ReuseHistory fromImage;
        fromImage.valid = false;
        fromImage.value = 0;
        fromImage.moment = fromImage.frames = 0;
        float2 prevPixel;
        float prevDepth;
        if (reuseDominantDirection(s.roughness) > 0.5)
        {
            const float3 imagePoint = g_cameraPosition - toEye / max(eyeDistance, 1e-20) * (eyeDistance + centre.a);
            if (giPreviousPixel(imagePoint, float2(size), prevPixel, prevDepth)) fromImage = reuseHistoryAt(prevPixel, float2(size), false, 0, 0, range);
        }
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[0].w, P[1].x, pixel, s.position, s.normal, prevP, prevN, instance);
        ReuseHistory fromSurface;
        fromSurface.valid = false;
        fromSurface.value = 0;
        fromSurface.moment = fromSurface.frames = 0;
        if (giPreviousPixel(prevP, float2(size), prevPixel, prevDepth))
        {
            const float noise = reuseUnit(pixel.x + pixel.y * 65536u + (P[4].z & 7u) * 0x9E3779B9u);
            const float tolerance = prevDepth * asfloat(P[4].y) * lerp(0.5, 1.5, noise) / clamp(saturate(dot(s.view, s.normal)), 0.1, 1.0);
            fromSurface = reuseHistoryAt(prevPixel, float2(size), true, prevDepth, tolerance, range);
        }
        if (fromImage.valid || fromSurface.valid)
        {
            // this frame's neighbourhood (YCoCg of the filters' space)
            float3 m1 = 0, m2 = 0;
            float count = 0;
            [loop] for (int dy = -2; dy <= 2; ++dy)
            {
                [loop] for (int dx = -2; dx <= 2; ++dx)
                {
                    if (abs(dx) == 2 && abs(dy) == 2) continue;
                    const int2 q = int2(pixel) + int2(dx, dy);
                    if (any(q < 0) || any(q >= int2(size))) continue;
                    float4 v = centre;
                    if (dx != 0 || dy != 0) v = resolved.Load(int3(q, 0));
                    if (v.a < 0) continue;
                    const float3 y = reuseToYCoCg(reuseToFilter(v.rgb, range));
                    m1 += y;
                    m2 += y * y;
                    count += 1;
                }
            }
            m1 /= count;
            const float3 extent = asfloat(P[3].w) * sqrt(max(m2 / count - m1 * m1, 0.0));
            const float3 imageY = reuseToYCoCg(fromImage.value), surfaceY = reuseToYCoCg(fromSurface.value);
            const float3 imageClamped = clamp(imageY, m1 - extent, m1 + extent), surfaceClamped = clamp(surfaceY, m1 - extent, m1 + extent);
            float surfaceShare = length(m1 - imageY) > length(m1 - surfaceY) + 0.01 ? 0.8 : 0.0;
            if (!fromImage.valid) surfaceShare = 1;
            else if (!fromSurface.valid) surfaceShare = 0;
            const float3 historyY = lerp(imageY, surfaceY, surfaceShare), clampedY = lerp(imageClamped, surfaceClamped, surfaceShare);
            // how far the history lay outside the neighbourhood, in its extents: it then counts for fewer frames
            float confidence = saturate(1 - length(abs(clampedY - historyY) / max(extent, 0.1)));
            confidence = 0.75 * confidence + 0.25;
            const float had = surfaceShare > 0.5 ? fromSurface.frames : fromImage.frames;
            frames = min(had * confidence + 1, maxFrames);
            const float blend = 1 / frames;
            value = lerp(reuseFromYCoCg(clampedY), value, blend);
            moment = lerp(lerp(fromImage.moment, fromSurface.moment, surfaceShare), moment, blend);
        }
    }
    // (frames stays 0 on a frame without history: the filter widens there, and the next frame's blend 1 / (0 + 1) takes
    // its own value whole - a single unaccumulated frame is no history yet)
    value = max(value, 0.0);
    bool finite = !(any(isnan(value)) || any(isinf(value)) || isnan(moment) || isinf(moment));
    if (!finite)
    {
        value = 0;
        moment = 0;
        frames = 0;
    }
    valueOut[pixel] = float4(reflStorable(reuseFromFilter(value, range)), min(max(moment, 0.0), 65504.0));
    framesOut[pixel] = float4(frames, 0, 0, 0);
    keysOut[pixel] = uint2(finite ? 1u : 0u, asuint(s.linearDepth));
}
