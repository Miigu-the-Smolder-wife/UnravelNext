// unx-kernel: cs_6_6 main
// m.upscale with output.upscale_tsr (Tsr.hlsli; the reference's TSRUpdateHistory, update quality 3): the history
// pixel's value from the internal samples around it and its reprojected history. The history is at the output's
// resolution or above it (output.upscale_tsr_history_percent; m.tsr.resolve then filters it down): "O" below is a
// history pixel, and the history's sample counts are per history pixel - 16 of them over an output pixel.
//   samples    the input pixel K under the output pixel O, its neighbours on O's side along each axis, and per axis the
//              corner on O's side where O is more than a quarter pixel off K's centre, else the neighbour on the far
//              side. Weights: tsrSampleWeight under the kernel factor - 1 (an input pixel wide; less by the pixel's
//              noise) where the history is missing or rejected, output / input size where it refines - times the tone
//              weight. Where the history is rejected or missing O first moves by the spatial anti-aliaser's offset.
//   history    Catmull-Rom at O less the dilated reprojection vector, times the exposure ratio; clamped to the samples'
//              range except as far as the rejection pass found it consistent (history clamp disable).
//   resurrected (bit 1 of the rejection's a; m.tsr.reject) the history is the kept frame's (P[4].x): O's own place in
//              it by the cameras - the closest depth of the pixel the rejection came from, P[5..8] - and that frame's
//              exposure; the pixel is not disoccluded.
//   field      (flag 4; m.tsr.dilate's reprojection field) on a boundary K's vector, jacobian and rejection are read at
//              the neighbour on O's side of it: the foreground's (the dilation's offset) where O reaches over the
//              boundary, else the pixel across; the vector is carried from that pixel's centre to O by the jacobian;
//              the rejection is the stricter of K's and that pixel's. A reprojection that magnifies the history (the
//              jacobian's upscale factor) holds the history's weight down as a rejection of 1 / that factor would.
//   weights   the history's validity (alpha, 1 = 16 samples) capped where rejected (2 samples), the input's weight =
//              its alignment with O x 1 / 16, the history's = the input's x (1 - b) / b for the rejection's blend
//              factor b, at most the validity, at most 1 - 0.75 x speed in output pixels a frame (not below the
//              relative luma change: high-contrast edges stay stable in motion).
// P[0] = { colour SRV (internal, exposed linear), rejection SRV (RGBA8, TsrReject.hlsl), dilated motion SRV (RG32F;
//          with output.upscale_tsr_hole_filling the decimate's copy, a disoccluded pixel's vector its occluder's),
//          history SRV (output: rgb exposed linear, a = validity) }
// P[1] = { output UAV (RGBA16F), internal width, height, flags (1: reset; 2: the kernel narrows with the samples
//          gathered - output.upscale_tsr_kernel_by_samples; 4: the reprojection field's jacobian and boundary apply;
//          8: the history is the lens picture - output.lens_panini_d, Lens.hlsli: a history pixel takes its samples and
//          its vector at its place in the rendered picture, and reads the history at the previous place's place under
//          that frame's lens) }
// P[2] = { history width, height, asuint(jitter x), asuint(jitter y) }, P[3] = { asuint(exposure ratio), AA SRV (RG8_UINT),
//          field SRV (RGBA32_UINT, Tsr.hlsli; 0xFFFFFFFF: none), 0 }
// P[4] = { kept frame's history SRV (0xFFFFFFFF: no resurrection this frame), asuint(this frame's exposure over the
//          kept frame's), asuint(history size / output size), 0 }, P[5..8] = rows of this frame's unjittered clip
//          space to the kept frame's
// P[9] = asfloat { the lens (Lens.hlsli): tan half field of view x, y, d, s }, P[10] = asfloat { its scale, the previous
//         frame's tan x, tan y, scale }, P[11] = asfloat { the kept frame's tan x, tan y, scale, 0 }
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Shading/Lens.hlsli"

float previousWeightMultiplier(float blendFactor) { return (1.0 - blendFactor) / max(blendFactor, 1.0 / 1024.0); }

[numthreads(8, 8, 1)]
void main(uint2 o : SV_DispatchThreadID)
{
    const uint2 outSize = P[2].xy;
    if (any(o >= outSize)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> rejectionTexture = ResourceDescriptorHeap[P[0].y];
    Texture2D<float2> motionTexture = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> aaTexture = ResourceDescriptorHeap[P[3].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const int2 inSize = int2(P[1].yz);
    const float2 jitter = asfloat(P[2].zw);
    const float inputToHistory = (float)outSize.x / (float)inSize.x;
    // the history's sample counts per history pixel (the reference's HistorySampleCount, HistoryHisteresis,
    // WeightClampingPixelSpeedAmplitude, InvWeightClampingPixelSpeed under its history screen percentage)
    const float historyScale = asfloat(P[4].z);
    const float historySamples = TSR_HISTORY_SAMPLES / (historyScale * historyScale), hysteresis = 1.0 / historySamples;
    const float speedAmplitude = saturate(1.0 - 4.0 * hysteresis), invClampingSpeed = TSR_INV_WEIGHT_CLAMPING_PIXEL_SPEED / historyScale;
    // (uv: the history pixel's place in the rendered picture - under a lens not its place in the history)
    const bool lensOn = (P[1].w & 8u) != 0;
    const float4 lens = asfloat(P[9]);
    const float2 uvOut = (float2(o) + 0.5) / float2(outSize);
    const float2 uv = lensOn ? lensToRendered(uvOut, lens, asfloat(P[10].x)) : uvOut;
    // O in the input's pixel coordinates (sample k covers [k, k + 1)) and the input pixel K under it
    float2 ppo = uv * float2(inSize) + jitter;
    int2 k = clamp(int2(floor(ppo)), 0, inSize - 1);

    // the reprojection field: the input pixel the vector is read at and the vector's change from its centre to O
    int2 kv = k;
    float2 correction = 0;
    float upscaleCorrection = 1;
    if ((P[1].w & 4u) != 0)
    {
        Texture2D<uint4> fieldTexture = ResourceDescriptorHeap[P[3].z];
        const float2 fromCentre = ppo - (floor(ppo) + 0.5);
        const uint boundary = fieldTexture.Load(int3(k, 0)).y;
        int2 dilateOffset = 0;
        if (boundary != TSR_NO_BOUNDARY)
        {
            const int2 offset = tsrDecodeBoundaryOffset(boundary);
            dilateOffset = tsrInsideBoundary(fromCentre, tsrDecodeBoundary(boundary), 1.0 / inputToHistory) ? offset : -offset;
        }
        kv = clamp(k + dilateOffset, 0, inSize - 1);
        float2 dx, dy;
        tsrDecodeJacobian(fieldTexture.Load(int3(kv, 0)).x, dx, dy);
        const float2 coordinate = fromCentre - float2(dilateOffset);
        correction = (coordinate.x * dx + coordinate.y * dy) / float2(inSize);
        upscaleCorrection = 1.0 / max(tsrJacobianUpscale(dx, dy), 1.0);
    }
    float4 rejection = rejectionTexture.Load(int3(k, 0));
    if (any(kv != k))
    {
        const float4 across = rejectionTexture.Load(int3(kv, 0));
        rejection = float4(min(rejection.rg, across.rg), max(rejection.b, across.b), across.a);
    }
    const float lowFrequencyRejection = rejection.r, disableHistoryClamp = rejection.g, decreaseValidity = rejection.b;
    const uint rejectionBits = (uint)round(rejection.a * 255.0);
    const bool parallaxRejected = (rejectionBits & 1u) == 0;
    const bool resurrected = (rejectionBits & 2u) != 0 && P[4].x != 0xFFFFFFFFu;
    if ((rejectionBits & 4u) != 0)
    {
        // a hole-filled vector (TsrDecimate.hlsl) is the occluder's: the pixel's own jacobian says nothing about it
        correction = 0;
        upscaleCorrection = 1;
    }
    const uint2 aa = aaTexture.Load(int3(k, 0));
    const float noiseFiltering = (float)aa.y / 255.0;
    float2 vector = motionTexture.Load(int3(kv, 0)) + correction;
    float historyExposure = asfloat(P[3].x);
    uint historySrv = P[0].w;
    if (resurrected)
    {
        Texture2D<uint4> fieldTexture = ResourceDescriptorHeap[P[3].z];
        const float4x4 toKept = float4x4(asfloat(P[5]), asfloat(P[6]), asfloat(P[7]), asfloat(P[8]));
        const float4 clip = mul(toKept, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, asfloat(fieldTexture.Load(int3(kv, 0)).z), 1.0));
        vector = clip.w > 1e-6 ? uv - float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : float2(2, 2);
        historyExposure = asfloat(P[4].y);
        historySrv = P[4].x;
        upscaleCorrection = 1;
    }
    float2 previousUv = uv - vector;
    if (lensOn)
    {
        // (that frame's own lens: the field of view may have changed since)
        const float3 then = resurrected ? asfloat(P[11].xyz) : asfloat(P[10].yzw);
        previousUv = renderedToLens(previousUv, float4(then.xy, lens.zw), then.z);
    }
    const bool offScreen = (P[1].w & 1u) != 0 || any(previousUv <= 0) || any(previousUv >= 1);
    const bool disoccluded = !offScreen && parallaxRejected && !resurrected;
    const bool noHistory = offScreen || disoccluded;
    const float outputVelocity = length(vector * float2(outSize));

    // the history
    float3 previous = 0;
    float previousValidity = 0;
    if (!(lowFrequencyRejection <= 0 || offScreen))
    {
        Texture2D<float4> history = ResourceDescriptorHeap[historySrv];
        float4 h = tsrCatmullRom(history, previousUv, float2(outSize));
        if (!all(isfinite(h)) || any(h.rgb < 0)) h = history.SampleLevel(g_linearClamp, previousUv, 0);  // (the kernel's negative lobes at a highlight)
        if (all(isfinite(h)))
        {
            previous = max(h.rgb * historyExposure, 0.0);
            previousValidity = saturate(h.a);
        }
    }

    // the spatial anti-aliaser's offset where the history does not hold
    ppo += tsrDecodeAaOffset(aa.x) * (noHistory ? 1.0 : saturate(1.0 - lowFrequencyRejection * 4.0));
    const float2 ppk = floor(ppo) + 0.5;
    k = clamp(int2(floor(ppo)), 0, inSize - 1);
    const float2 dKO = ppo - ppk;

    // the kernel's width and the weights
    float kernelFactor, currentWeight, previousWeight;
    {
        const float rejectionBlend = offScreen ? 1.0 : 1.0 - min(lowFrequencyRejection, upscaleCorrection);
        const float coarseContribution = tsrSampleWeight(1.0, dKO, 0.0) * hysteresis;
        const float idealContribution = tsrSampleWeight(inputToHistory, dKO, 0.0) * hysteresis;
        const float validity = min(previousValidity, 1.0 - TSR_WEIGHT_CLAMPING_REJECTION * decreaseValidity);
        const float refiningHysteresis = validity + idealContribution > 0 ? idealContribution / (validity + idealContribution) : 1.0;
        const float coarseRejected = min(coarseContribution * previousWeightMultiplier(rejectionBlend), 1.0);
        const float coarseRefining = min(coarseContribution * previousWeightMultiplier(refiningHysteresis), 1.0);
        const float refining = min(coarseRejected < coarseRefining ? 0.0 : 1.0, saturate(validity * historySamples));
        // (flag 2) n samples gathered over an input pixel lie about 1 / sqrt(n) input pixels apart: a kernel narrower than
        // that leaves the output pixels between them on the older, wider estimate - the input's lattice shows on small
        // bright detail in the frames after a cut. The kernel factor follows sqrt(n) up to the output pixel's.
        const float gathered = sqrt(max(validity * TSR_HISTORY_SAMPLES, 1.0));
        const float filled = (P[1].w & 2u) != 0 ? saturate((gathered - 1.0) / max(inputToHistory - 1.0, 1e-3)) : 1.0;
        const float kernelLerp = noHistory ? 0.0 : saturate(lowFrequencyRejection * 16.0 - 13.0) * refining * filled;
        kernelFactor = lerp(1.0 - 0.5 * noiseFiltering, inputToHistory, kernelLerp);
        currentWeight = tsrSampleWeight(lerp(1.0, inputToHistory, kernelLerp), dKO, 0.0) * hysteresis;
        previousWeight = min((currentWeight > 0 ? currentWeight : coarseContribution) * previousWeightMultiplier(rejectionBlend), validity);
        previousWeight = min(previousWeight, 1.0 - currentWeight);
    }

    // the five samples
    float3 filtered = 0, colourMin = 0, colourMax = 0;
    {
        const float2 sign = float2(dKO.x < 0 ? -1.0 : 1.0, dKO.y < 0 ? -1.0 : 1.0);
        const bool2 far = abs(dKO) >= 0.25;  // (a corner on O's side, else the neighbour across)
        float2 offsets[5];
        offsets[0] = float2(0, 0);
        offsets[1] = float2(sign.x, 0);
        offsets[2] = float2(0, sign.y);
        offsets[3] = far.x ? sign : float2(-sign.x, 0);
        offsets[4] = far.y ? sign : float2(0, -sign.y);
        float weightSum = 0;
        [unroll] for (uint s = 0; s < 5; ++s)
        {
            float3 c = colour.Load(int3(clamp(k + int2(offsets[s]), 0, inSize - 1), 0)).rgb;
            c = all(isfinite(c)) ? max(c, 0.0) : float3(0, 0, 0);
            float weight = tsrSampleWeight(kernelFactor, offsets[s] - dKO, 0.005);
            if (s == 4 && far.x && far.y) weight = 0;  // (the same corner twice)
            weight *= tsrHdrWeight(c);
            filtered += c * weight;
            weightSum += weight;
            colourMin = s == 0 ? c : min(colourMin, c);
            colourMax = s == 0 ? c : max(colourMax, c);
        }
        filtered /= max(weightSum, 1e-12);
    }

    // the history clamped only as far as the rejection asks
    float3 blended;
    {
        const float3 clamped = clamp(previous, colourMin, colourMax);
        float a = (1.0 - disableHistoryClamp) * tsrHdrWeight(clamped), b = disableHistoryClamp * tsrHdrWeight(previous);
        const float inverse = a + b > 0 ? 1.0 / (a + b) : 0.0;
        blended = clamped * (a * inverse) + previous * (b * inverse);
    }
    // less history where the pixel moves, unless the luma would jump
    {
        float maxValidity = 1.0 - speedAmplitude * saturate(outputVelocity * invClampingSpeed);
        const float lumaPrevious = tsrLuma4(blended), lumaFiltered = tsrLuma4(filtered);
        maxValidity = max(maxValidity, abs(lumaFiltered - lumaPrevious) / max(max(lumaFiltered, lumaPrevious), 1e-12));
        previousWeight = min(previousWeight, maxValidity);
    }
    const float validity = currentWeight + previousWeight;
    const float blendPrevious = previousWeight * tsrHdrWeight(blended), blendCurrent = currentWeight * tsrHdrWeight(filtered);
    const float common = blendPrevious + blendCurrent > 0 ? 1.0 / (blendPrevious + blendCurrent) : 0.0;
    float3 result = blended * (blendPrevious * common) + filtered * (blendCurrent * common);
    if (!(common > 0)) result = filtered;
    result = all(isfinite(result)) ? clamp(result, 0.0, 65504.0) : float3(0, 0, 0);
    output[o] = float4(result, ceil(255.0 * validity) / 255.0);
}
