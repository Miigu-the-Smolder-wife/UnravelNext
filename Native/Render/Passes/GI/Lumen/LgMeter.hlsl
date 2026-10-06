// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.meter.*: the exposure the ray intensity cap refers to on a snap frame (gi.lumen_cap_snap_exposure).
// The cap of LgComposite is a bound on a trace's exposed radiance. On a snap frame (the renderer's first frames, a cut
// or restore until a histogram of the new view came back: Exposure.cpp) the frame's exposure was not metered on what
// it shows - the output corrects that afterwards - so the cap is too strict by the exposure's error after a cut into a
// brighter view (darker indirect light for the pixel history's length) and without effect in the first frames (EV 14)
// and after a cut into a darker view (fireflies). Here the frame meters its own traces instead: the radiance a probe's
// rays met is the radiance of the surfaces around it, the quantity the view's pixels show. A probe's value is the mean
// of its traces (by their share of the sphere - a pixel's luminance is such a mean, not one ray), weighted by its
// pixel's distance from the view's centre as the exposure histogram weights pixels; the histogram is metered as the
// exposure meter does (Exposure.cpp meterEv100: the mean of log2 L between the dark and bright cuts onto the target
// grey, less the compensation, clamped), and the cap is taken in that exposure. Other frames keep the frame's own
// exposure (Unreal: the pre-exposure of last frame's adaptation, which the display follows too).
// Three dispatches of this kernel, P[0].w = mode: 0 clear the histogram (1 group), 1 histogram (a group per probe,
// its threads load the probe's 64 traces together), 2 meter (1 group).
// P[0] = { trace radiance SRV (x g_exposure), histogram (raw 64 x uint; UAV, SRV in mode 2), reference UAV (mode 2;
// raw: float c = exposure(metered) / g_exposure, float metered EV100, uint metered, 0), mode }
// P[1] = { asfloat target grey, asfloat cut dark, asfloat cut bright, asfloat exposure compensation }
// P[2] = { asfloat min EV100, asfloat max EV100, asfloat centre sigma (half view heights), ray info SRV (mode 1) },
// P[10].z adaptive SRV, P[10].w probe depth SRV.
#include "Passes/GI/Lumen/LgCommon.hlsli"

#define LG_METER_BINS 64u
#define LG_METER_LOG2_MIN -8.0   // the exposure histogram's bins (Exposure.h kExposureLog2Min / Step)
#define LG_METER_LOG2_STEP 0.5

// Preserve the original ordered accumulation; parallelize the independent loads
// and ray decoding instead of changing the floating-point reduction order.
groupshared float2 gs_meterTrace[LG_TRACE_RES * LG_TRACE_RES];
groupshared uint gs_meterHistogram[LG_METER_BINS];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint mode = P[0].w;
    const uint index = thread.y * 8 + thread.x;
    if (mode == 0)
    {
        RWByteAddressBuffer histogram = ResourceDescriptorHeap[P[0].y];
        histogram.Store(4 * index, 0u);
        return;
    }
    if (mode == 1)
    {
        const uint2 atlas = group.xy;
        const uint probe = lgProbeIndex(atlas);
        ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
        Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
        if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive) || !(probeDepth[atlas] > 0)) return;
        Texture2D<float4> traceRadiance = ResourceDescriptorHeap[P[0].x];
        Texture2D<uint> rayInfo = ResourceDescriptorHeap[P[2].w];
        RWByteAddressBuffer histogram = ResourceDescriptorHeap[P[0].y];
        [loop] for (uint i = index; i < LG_TRACE_RES * LG_TRACE_RES; i += 64u)
        {
            const uint2 coord = atlas * LG_TRACE_RES + uint2(i % LG_TRACE_RES, i / LG_TRACE_RES);
            uint2 rayTexel;
            uint level;
            lgUnpackRay(rayInfo[coord], rayTexel, level);
            const float mapSize = (float)((LG_TRACE_RES * 2) >> level);
            const float share = ((float)LG_GATHER_RES / mapSize) * ((float)LG_GATHER_RES / mapSize);
            gs_meterTrace[i] = float2(dot(traceRadiance[coord].rgb, float3(0.2126, 0.7152, 0.0722)), share);
        }
        GroupMemoryBarrierWithGroupSync();
        if (index != 0) return;
        float sum = 0, shares = 0;
        [loop] for (uint i = 0; i < LG_TRACE_RES * LG_TRACE_RES; ++i)
        {
            sum += gs_meterTrace[i].x * gs_meterTrace[i].y;
            shares += gs_meterTrace[i].y;
        }
        const float luminance = sum / max(shares, 1e-6) / max(g_exposure, 1e-20);
        if (!(luminance > 0)) return;
        const float sigma = asfloat(P[2].z);
        const float2 d = (float2(lgProbePixel(adaptive, probe)) + 0.5 - 0.5 * float2(lgViewSize())) / (0.5 * (float)lgViewSize().y);
        const uint weight = (uint)(64.0 * exp(-0.5 * dot(d, d) / (sigma * sigma)) + 0.5);
        if (weight == 0) return;
        const uint bin = (uint)clamp((log2(luminance) - LG_METER_LOG2_MIN) / LG_METER_LOG2_STEP, 0.0, (float)(LG_METER_BINS - 1u));
        histogram.InterlockedAdd(4 * bin, weight);
        return;
    }
    ByteAddressBuffer histogram = ResourceDescriptorHeap[P[0].y];
    gs_meterHistogram[index] = histogram.Load(4 * index);
    GroupMemoryBarrierWithGroupSync();
    if (index != 0) return;
    RWByteAddressBuffer reference = ResourceDescriptorHeap[P[0].z];
    const float targetGrey = asfloat(P[1].x), cutLow = asfloat(P[1].y), cutHigh = asfloat(P[1].z), compensation = asfloat(P[1].w);
    const float evUsed = -log2(1.2 * max(g_exposure, 1e-20));
    float total = 0;
    [loop] for (uint b = 0; b < LG_METER_BINS; ++b) total += (float)gs_meterHistogram[b];
    const float lo = total * cutLow, hi = total * (1.0 - cutHigh);
    float below = 0, sum = 0, weight = 0;
    [loop] for (uint k = 0; k < LG_METER_BINS; ++k)
    {
        const float c = (float)gs_meterHistogram[k], a = max(below, lo), z = min(below + c, hi);
        if (z > a)
        {
            sum += (z - a) * (LG_METER_LOG2_MIN + (k + 0.5) * LG_METER_LOG2_STEP);
            weight += z - a;
        }
        below += c;
    }
    if (!(weight > 0))
    {
        reference.Store4(0, uint4(asuint(1.0), asuint(evUsed), 0, 0));  // nothing metered: the frame's own exposure
        return;
    }
    const float metered = clamp(sum / weight - log2(1.2 * targetGrey) - compensation, asfloat(P[2].x), asfloat(P[2].y));
    reference.Store4(0, uint4(asuint(exp2(evUsed - metered)), asuint(metered), 1, 0));
}
