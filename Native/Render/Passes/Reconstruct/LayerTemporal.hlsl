// unx-kernel: cs_6_6 main
// Time assistance of the reflection layers (RENDERER_REDESIGN_V2 1.2; LayerCommon.hlsli), after the spatial
// reconstruction (LayerDenoise), one thread per pixel of the view. The spatially reconstructed layers are already the
// frame's value; the history only removes what the filter leaves: a running mean over at most
// reflection.layer_history_frames frames (8), weight 1 / (n + 1), of the reconstructed layers found where the same
// content was last frame:
//   M pixels: the layer is the reflected surface's lighting, so it is followed as the reflected image moves, not as the
//     mirror does: the image point (the eye ray continued behind the mirror for the hit distance - exact for a planar
//     mirror and a static reflected point) projected with the previous view. A tap is the same content when it was an M
//     pixel whose image lay at that point's distance from the previous eye (2 %) with the same hit normal (cos > 0.9).
//     The mirror's identity is not in the layer, so the history may follow a moving camera (ReflectionAccumulate could
//     integrate a mirror's value only while the view stood still).
//   G pixels: the pixel's own surface point one tick ago (GiScreenHistory.hlsli: exact motion), where the tap holds the
//     same instance on the point's plane. The window is limited as ReflectionAccumulate's: the lobe integral depends on
//     the view, so the reflected direction may travel reflection.temporal_lobe_shift of the lobe half-angle over it.
//   Both: the hits' motion (ReflectionResolve's history, displacement per tick over the ray footprint) limits the window
//     to n x motion <= reflection.temporal_lobe_shift.
// A history value outside this frame's reconstruction is brought to its bound: per channel the mean +- 3 standard
// deviations of the reconstructed layer over the pixel's 3 x 3 neighbours of its mode, widened by 5 % of the value. Where
// the spatial filter did its work the neighbourhood is smooth and the bound tight - a lighting change is followed in the
// frame it happens, at the reconstruction's own noise; where it found no neighbours to average (hit geometry that
// differs from pixel to pixel) the neighbourhood still carries the frame's noise, the bound is as wide as that noise and
// the history does the averaging. (The filter's own sigma is no bound: a pixel without accepted taps reports 0 and
// would lose its history exactly where it is all the pixel has - measured, the first run.)
// The outputs are both this frame's reconstructed layers (LayerCompose reads them) and the next frame's history: rgb and
// a = the frames in it. Keys: x = mode | (M: hit normal oct 8 + 8; G: scene instance + 1) << 2, y = float bits (M: the
// image's distance from the eye; G: linear depth).
// P[0] = { stochastic SRV, residual SRV (LayerDenoise's last level), guide SRV, vis id SRV }
// P[1] = { visible clusters SRV, previous stochastic SRV, previous residual SRV, previous keys SRV }
// P[2] = { stochastic out UAV, residual out UAV, keys out UAV, history frames }
// P[3] = { width, height, asuint(lobe shift), flags (bit 0: no history this frame) }
// P[4] = { asuint(previous camera position xyz), asuint(pixel angle) }, P[5] = { hit distance / motion history SRV, 0, 0, 0 }
// Frame constants b1 = main view.
#include "Passes/Reconstruct/LayerCommon.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

// The history brought within the frame's bound: mean +- (3 deviation + 5 % of |current|) per channel.
float3 layerBoundHistory(float3 history, float3 current, float3 mean, float3 deviation)
{
    const float3 bound = 3 * deviation + 0.05 * abs(current) + 1e-6;
    return clamp(history, min(mean, current) - bound, max(mean, current) + bound);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy;
    if (any(pixel >= size)) return;
    Texture2D<uint4> guides = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint2> keysOut = ResourceDescriptorHeap[P[2].z];
    const uint4 g0 = guides.Load(int3(pixel, 0));
    if (layerMode(g0) == LAYER_MODE_NONE)
    {
        keysOut[pixel] = uint2(0, 0);
        return;
    }
    Texture2D<float4> inS = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> inR = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> outS = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<float4> outR = ResourceDescriptorHeap[P[2].y];
    const LayerGuide c = layerGuide(g0, pixel);
    const bool mirror = c.mode == LAYER_MODE_M;
    const float4 s = inS.Load(int3(pixel, 0));
    float4 r = 0;
    if (!mirror) r = inR.Load(int3(pixel, 0));
    const float3 toEye = g_cameraPosition - c.position;
    const float eyeDistance = length(toEye);
    const float3 view = toEye / max(eyeDistance, 1e-20);
    const float3 prevCamera = asfloat(P[4].xyz);
    const float pixelAngle = asfloat(P[4].w);
    Texture2D<float2> hitHistory = ResourceDescriptorHeap[P[5].x];
    const float motion = hitHistory.Load(int3(pixel, 0)).y;  // ReflectionResolve: the value's hit motion
    float window = motion > 0 ? asfloat(P[3].z) / motion : 1e9;
    // The content's position last frame, what its key was there, and the tolerance of the key's depth.
    float2 prevPixel = 0;
    uint keyX = 0;
    float keyDepth = 0, expected = 0, tolerance = 0;
    bool found = false;
    if (mirror)
    {
        const float image = eyeDistance + c.hitDistance;
        keyX = LAYER_MODE_M | ((g0.z & 0xFFFFu) << 2);
        keyDepth = image;
        const float3 imagePoint = g_cameraPosition - view * image;
        float prevDepth;
        found = giPreviousPixel(imagePoint, float2(size), prevPixel, prevDepth);
        expected = distance(imagePoint, prevCamera);
        tolerance = 0.02 * expected;
    }
    else
    {
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[0].w, P[1].x, pixel, c.position, c.normal, prevP, prevN, instance);
        keyX = LAYER_MODE_G | (instance << 2);
        keyDepth = c.linearZ;
        found = instance != 0 && giPreviousPixel(prevP, float2(size), prevPixel, expected);
        tolerance = expected * (1e-3 + 2 * pixelAngle / max(abs(dot(c.normal, view)), 0.1));
        // the reflected direction's travel since last frame against the lobe half-angle (ReflectionAccumulate's rule)
        const float3 refl = reflect(-view, c.normal), reflPrev = reflect(-normalize(prevCamera - prevP), prevN);
        const float shift = 2 * asin(saturate(0.5 * length(refl - reflPrev)));
        const float lobe = reflectionLobeHalfAngle(c.roughness, max(dot(c.normal, view), 1e-4));
        if (shift > 0) window = min(window, asfloat(P[3].z) * lobe / shift);
    }
    keysOut[pixel] = uint2(keyX, asuint(keyDepth));
    uint n = 0;
    float3 meanS = s.rgb, meanR = r.rgb;
    if (found && (P[3].w & 1u) == 0)
    {
        Texture2D<float4> prevS = ResourceDescriptorHeap[P[1].y];
        Texture2D<float4> prevR = ResourceDescriptorHeap[P[1].z];
        Texture2D<uint2> prevKeys = ResourceDescriptorHeap[P[1].w];
        const float2 at = prevPixel - 0.5;  // (giPreviousPixel: pixel centres at + 0.5)
        const int2 i0 = int2(floor(at));
        const float2 f = at - floor(at);
        float3 sumS = 0, sumR = 0;
        float weight = 0;
        uint nPrev = 0xFFFFFFFFu;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = i0 + o;
            if (any(t < 0) || any(t >= int2(size))) continue;
            const uint2 key = prevKeys.Load(int3(t, 0));
            const float4 a = prevS.Load(int3(t, 0));
            if (abs(asfloat(key.y) - expected) > tolerance) continue;
            if (mirror)
            {
                if ((key.x & 3u) != LAYER_MODE_M || dot(c.hitNormal, reflUnpackOct16(key.x >> 2)) < 0.9) continue;
            }
            else if (key.x != keyX) continue;
            const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
            if (w <= 0) continue;
            sumS += w * a.rgb;
            if (!mirror) sumR += w * prevR.Load(int3(t, 0)).rgb;
            weight += w;
            nPrev = min(nPrev, (uint)a.a);
        }
        if (weight > 0 && nPrev != 0xFFFFFFFFu && nPrev > 0)
        {
            n = (uint)min((float)min(nPrev, max(P[2].w, 1u) - 1), floor(window));
            if (n > 0)
            {
                // the reconstructed layers' mean and deviation over the 3 x 3 neighbours of the pixel's mode
                float3 m1S = 0, m2S = 0, m1R = 0, m2R = 0;
                float count = 0;
                [loop] for (int k9 = 0; k9 < 9; ++k9)
                {
                    const int2 q = int2(pixel) + int2(k9 % 3, k9 / 3) - 1;
                    if (any(q < 0) || any(q >= int2(size))) continue;
                    if (k9 != 4 && layerMode(guides.Load(int3(q, 0))) != c.mode) continue;
                    const float3 a = k9 == 4 ? s.rgb : inS.Load(int3(q, 0)).rgb;
                    m1S += a;
                    m2S += a * a;
                    if (!mirror)
                    {
                        const float3 b = k9 == 4 ? r.rgb : inR.Load(int3(q, 0)).rgb;
                        m1R += b;
                        m2R += b * b;
                    }
                    count += 1;
                }
                m1S /= count, m1R /= count;
                const float3 devS = sqrt(max(m2S / count - m1S * m1S, 0.0)), devR = sqrt(max(m2R / count - m1R * m1R, 0.0));
                meanS = lerp(layerBoundHistory(sumS / weight, s.rgb, m1S, devS), s.rgb, 1.0 / (n + 1));
                meanR = lerp(layerBoundHistory(sumR / weight, r.rgb, m1R, devR), r.rgb, 1.0 / (n + 1));
            }
        }
    }
    if (any(isnan(meanS)) || any(isinf(meanS)) || any(isnan(meanR)) || any(isinf(meanR)))
    {
        meanS = meanR = 0;
        n = 0;
        keysOut[pixel] = uint2(0, 0);  // (never expected: no history from this pixel)
    }
    outS[pixel] = float4(meanS, (float)(n + 1));
    if (!mirror) outR[pixel] = float4(meanR, (float)(n + 1));
}
