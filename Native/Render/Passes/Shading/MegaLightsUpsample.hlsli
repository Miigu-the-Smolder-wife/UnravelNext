// The lights of a pixel's light samples (MegaLights.hlsli, m.ml.shade): one of the 4 downsampled pixels around the pixel is
// drawn with probability proportional to its triangle-filter weight x plane weight x normal weight (so the mean over the
// draws is the edge-aware bilinear upsample), and its N samples are grouped by light.
#ifndef UNX_MEGA_LIGHTS_UPSAMPLE_HLSLI
#define UNX_MEGA_LIGHTS_UPSAMPLE_HLSLI
#include "Passes/Shading/MegaLights.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

struct MlPixelLights
{
    uint count;                    // distinct lights with a visible sample
    uint light[ML_MAX_SAMPLES];
    float weight[ML_MAX_SAMPLES];  // what the light's unshadowed radiance is multiplied by: its visible samples' capped weights / N
    float confidence;              // mean probability mass of the visible samples (1: no light reaches the pixel)
    bool valid;                    // false: none of the downsampled pixels holds this surface
};

MlPixelLights mlPixelLights(uint samplesSrv, uint keysSrv, uint2 pixel, float3 worldPos, float3 normal, float linearZ, uint factor, uint count,
                            float maxWeight, float maxWeightHidden)
{
    MlPixelLights o;
    o.count = 0;
    o.confidence = 1;
    o.valid = false;
    uint i;
    for (i = 0; i < ML_MAX_SAMPLES; ++i)
    {
        o.light[i] = ML_LIGHT_NONE;
        o.weight[i] = 0;
    }
    Texture2D<uint2> keys = ResourceDescriptorHeap[keysSrv];
    const uint2 dsSize = (uint2(g_viewWidth, g_viewHeight) + factor - 1) / factor;
    uint2 ds = pixel;
    if (factor >= 2)
    {
        const int2 ds00 = int2(floor((float2(pixel) + 0.5) / factor - 0.5));
        float4 w = 0;
        for (i = 0; i < 4; ++i)
        {
            const int2 c = ds00 + int2(i & 1, i >> 1);
            if (any(c < 0) || any(c >= int2(dsSize))) continue;
            const uint2 key = keys[c];
            const float z = asfloat(key.x);
            if (!(z > 0)) continue;
            const uint2 at = mlFullPixel(uint2(c), factor, g_frameIndex);
            const float2 d = abs(float2(at) - float2(pixel));
            float3 D, Dx, Dy;
            mPixelRay(float2(at) + 0.5, D, Dx, Dy);
            const float plane = dot(g_cameraPosition + D * z - worldPos, normal) / linearZ;
            const float angle = acos(saturate(dot(octDecode(key.y), normal)));
            const float nw = 1 - saturate(angle * (0.9 * 2 / 3.14159265));
            w[i] = max(2 - d.x, 0) * max(2 - d.y, 0) * exp2(-5000.0 * plane * plane) * nw * nw;
        }
        const float sum = w.x + w.y + w.z + w.w;
        if (!(sum >= 1e-4)) return o;
        const float u = mlNoise(pixel, g_frameIndex, 5) * sum;
        uint pick = 3;
        if (u <= w.x && w.x > 0) pick = 0;
        else if (u <= w.x + w.y && w.y > 0) pick = 1;
        else if (u <= w.x + w.y + w.z && w.z > 0) pick = 2;
        if (!(w[pick] > 0)) pick = w.x > 0 ? 0 : (w.y > 0 ? 1 : 2);
        ds = uint2(ds00 + int2(pick & 1, pick >> 1));
    }
    else if (!(asfloat(keys[ds].x) > 0)) return o;
    o.valid = true;

    Texture2D<uint2> samples = ResourceDescriptorHeap[samplesSrv];
    MlSample s[ML_MAX_SAMPLES];
    bool any = false;
    for (i = 0; i < ML_MAX_SAMPLES; ++i)
    {
        s[i] = mlNoSample();
        if (i < count) s[i] = mlUnpack(samples[mlSampleCoord(ds, count, i)]);
        any = any || s[i].light != ML_LIGHT_NONE;
    }
    float ratio = 0;
    int last = -1;
    for (uint k = 0; k < count; ++k)
    {
        // the next light in ascending order among the visible samples
        uint light = ML_LIGHT_NONE;
        for (i = 0; i < count; ++i)
            if (s[i].visible && int(s[i].light) > last) light = min(light, s[i].light);
        if (light == ML_LIGHT_NONE) break;
        last = int(light);
        float sum = 0, merged = 1;
        for (i = 0; i < count; ++i)
        {
            if (s[i].light != light) continue;
            if (s[i].merged)
            {
                merged += 1;
                continue;
            }
            if (s[i].visible)
            {
                sum += min(s[i].weight, s[i].guidedVisible ? maxWeight : maxWeightHidden) * merged;
                if (s[i].guidedVisible) ratio += merged / s[i].weight;
            }
            merged = 1;
        }
        o.light[o.count] = light;
        o.weight[o.count] = sum / count;
        ++o.count;
    }
    if (any) o.confidence = saturate(ratio / count);
    return o;
}
#endif
