// unx-kernel: cs_6_6 main
// Automatic exposure on a snap frame (Exposure.cpp exposureMeter): the first frames of a renderer and the frames after a
// cut or restore until the host has read a histogram of the new view (framesInFlight frames later) render with an EV that
// was not metered on that view. This kernel meters the frame's own histogram (complete after the shading kernels) as
// meterEv100 does - the weighted mean of log2 L between the dark and bright cuts onto the target grey, less the exposure
// compensation, clamped to [min, max] - and writes the correction the output applies to the exposed image:
// c = exposure(metered) / exposure(used) = 2^(EV used - EV metered). PostFinal multiplies the exposed HDR by c before
// its curve; histories keep the frame's own exposure (their next-frame ratio is the upscale's exposureRatio).
// P[0] = { histogram SRV (raw, 64 x uint), correction UAV (raw: float c, float metered EV100, uint metered, 0),
//          asfloat target grey, asfloat cut dark }
// P[1] = { asfloat cut bright, asfloat EV100 used, asfloat exposure compensation, 0 }
// P[2] = { asfloat min EV100, asfloat max EV100, 0, 0 }
#include "Bindless.hlsli"

#define EXPOSURE_BINS 64u
#define EXPOSURE_LOG2_MIN -8.0   // ShadingCommon.hlsli shExposureHistogram, Exposure.h kExposureLog2Min / Step
#define EXPOSURE_LOG2_STEP 0.5

groupshared uint gs_histogram[EXPOSURE_BINS];

[numthreads(64, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    ByteAddressBuffer histogram = ResourceDescriptorHeap[P[0].x];
    gs_histogram[index] = histogram.Load(4 * index);
    GroupMemoryBarrierWithGroupSync();
    // Keep the metering arithmetic and its accumulation order unchanged while
    // issuing the histogram loads together and reusing them for the second pass.
    if (index != 0) return;
    RWByteAddressBuffer correction = ResourceDescriptorHeap[P[0].y];
    const float targetGrey = asfloat(P[0].z), cutLow = asfloat(P[0].w), cutHigh = asfloat(P[1].x);
    const float evUsed = asfloat(P[1].y), compensation = asfloat(P[1].z);
    float total = 0;
    [loop] for (uint b = 0; b < EXPOSURE_BINS; ++b) total += (float)gs_histogram[b];
    if (!(total > 0))
    {
        correction.Store4(0, uint4(asuint(1.0), asuint(evUsed), 0, 0));  // nothing metered: the frame as rendered
        return;
    }
    const float lo = total * cutLow, hi = total * (1.0 - cutHigh);
    float below = 0, sum = 0, weight = 0;
    [loop] for (uint k = 0; k < EXPOSURE_BINS; ++k)
    {
        const float c = (float)gs_histogram[k], a = max(below, lo), z = min(below + c, hi);
        if (z > a)
        {
            sum += (z - a) * (EXPOSURE_LOG2_MIN + (k + 0.5) * EXPOSURE_LOG2_STEP);
            weight += z - a;
        }
        below += c;
    }
    if (!(weight > 0))
    {
        correction.Store4(0, uint4(asuint(1.0), asuint(evUsed), 0, 0));
        return;
    }
    // Exposure 1 / (1.2 x 2^EV100) maps the metered luminance onto the target grey: 2^EV100 = L / (1.2 grey).
    const float metered = clamp(sum / weight - log2(1.2 * targetGrey) - compensation, asfloat(P[2].x), asfloat(P[2].y));
    correction.Store4(0, uint4(asuint(exp2(evUsed - metered)), asuint(metered), 1, 0));
}
