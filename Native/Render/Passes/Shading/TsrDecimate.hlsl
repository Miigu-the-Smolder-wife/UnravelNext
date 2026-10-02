// unx-kernel: cs_6_6 main
// m.tsr.decimate (Tsr.hlsli; the reference's TSRDecimateHistory): per internal pixel,
//   parallax disocclusion  the closest occluders scattered around where the pixel was (m.tsr.dilate) against the
//                          pixel's own previous depth: a closer one - by more than three pixels' size in the world plus
//                          the pixel's depth error - means the pixel was hidden there. A closest occluder moving as the
//                          pixel moves is the pixel's own surface: no disocclusion;
//   reprojection edge      over the dilated vectors of the 3 x 3 neighbourhood: 0 where a neighbour's vector differs by
//                          a pixel along its offset (the history on either side of such an edge is another surface's);
//   guide                  the previous frame's guide reprojected (Catmull-Rom) with the exposure change applied;
//   flickering history     the previous frame's (TsrFlicker.hlsl), nearest texel at a position dithered within a texel.
// P[0] = { dilated motion SRV (RG32F), info SRV (RGBA16F, TsrDilate.hlsl), scatter SRV (R32_UINT), previous guide SRV
//          (R10G10B10A2: guide colour, a = uncertainty; UNX_NONE: no history) }
// P[1] = { reprojected guide UAV (R10G10B10A2), mask UAV (RG8: r = bits / 255 - 1 off screen or cut, 2 parallax
//          disocclusion; g = reprojection edge), width, height }
// P[2] = { asuint(jitter x), asuint(jitter y), asuint(exposure ratio), flags (1: reset - first frame, cut, restore) }
// P[3] = { previous flickering history SRV (RGBA8; UNX_NONE: none), reprojected flickering history UAV (RGBA8), frame, 0 }
// Frame constants b1 = the main view.
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 size = int2(P[1].zw);
    if (any(int2(id) >= size)) return;
    Texture2D<float2> dilated = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> infoTexture = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> scatter = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> guideOut = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float2> maskOut = ResourceDescriptorHeap[P[1].y];
    const float2 jitter = asfloat(P[2].xy);
    const float4 info = infoTexture.Load(int3(id, 0));
    const float previousZ = info.x, zError = info.y;
    const bool hasOffset = info.w > 0.75;

    // the reprojection edge over the neighbourhood's vectors (pixels)
    float2 v[9];
    [unroll] for (int k = 0; k < 9; ++k)
        v[k] = dilated.Load(int3(clamp(int2(id) + int2(k % 3, k / 3) - 1, 0, size - 1), 0)) * float2(size);
    float edge = 1;
    [unroll] for (int n = 0; n < 9; ++n)
    {
        if (n == 4) continue;
        const float2 o = float2(n % 3, n / 3) - 1.0;
        edge = min(edge, 1.0 - dot(saturate(abs((v[4] - v[n]) * o)), float2(1, 1)));
    }
    edge = saturate(edge * 1.1);
    edge = info.z > 0.9 ? info.z : edge;
    edge = hasOffset ? edge : 1.0;

    const float2 vector = v[4] / float2(size);
    const float2 uv = (float2(id) + 0.5 - jitter) / float2(size);
    const float2 previousUv = uv - vector;
    const bool offScreen = (P[2].w & 1u) != 0 || any(previousUv < 0) || any(previousUv > 1);

    // parallax disocclusion
    bool disoccluded = false;
    if (!offScreen)
    {
        const float depth = linearDepth(previousZ);
        const float depthError = abs(depth - linearDepth(previousZ + zError));
        const TsrBilinear b = tsrBilinear(float2(id) + 0.5 - v[4]);
        float mask = 0;
        uint closest = 0;
        bool anyValid = false;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const int2 texel = tsrBilinearTexel(b, i);
            if (any(texel < 0) || any(texel >= size)) continue;
            const uint sample = scatter.Load(int3(texel, 0));
            const float thereDepth = linearDepth(f16tof32(sample >> TSR_HOLE_BITS));
            const float epsilon = thereDepth * g_tanHalfFovY / g_viewHeight * 3.0 * 2.0 + depthError;
            mask += tsrBilinearWeight(b, i) * saturate(2.0 - abs(thereDepth - depth) / max(epsilon, 1e-12));
            closest = max(closest, sample);
            anyValid = true;
        }
        // (the closest occluder's own vector: where it equals this pixel's, the occluder is this pixel's surface)
        float holeAngle, holeLength;
        tsrDecodeHoleVelocity(closest, holeAngle, holeLength);
        if (anyValid && holeLength < tsrMaxHoleLength())
        {
            const float anglePrecision = 2.0 * 3.14159265 / 32.0;
            const float2 holeVelocity = float2(cos(holeAngle), sin(holeAngle)) * holeLength;
            const float velocityAngle = atan2(v[4].y, v[4].x), velocityLength = length(v[4]);
            const float lengthDifference = abs(holeLength - velocityLength) - 2.0;
            float angleDifference = abs(velocityAngle - holeAngle);
            angleDifference = min(angleDifference, 2.0 * 3.14159265 - angleDifference);
            const float polar = min(saturate(1.0 + 1.0 / TSR_HOLE_LENGTH_PRECISION - lengthDifference), saturate(2.0 - angleDifference / anglePrecision));
            const float cartesian = saturate(1.0 + anglePrecision * velocityLength - length(holeVelocity - v[4]));
            mask = max(mask, lerp(cartesian, polar, saturate(min(holeLength, velocityLength) - 2.0)));
        }
        disoccluded = mask < 0.5;
    }

    float4 guide = float4(0, 0, 0, 0);
    if (!offScreen && P[0].w != UNX_NONE)
    {
        Texture2D<float4> previousGuide = ResourceDescriptorHeap[P[0].w];
        guide = saturate(tsrCatmullRom(previousGuide, previousUv, float2(size)));
        guide.rgb = saturate(tsrLinearToGuide(tsrGuideToLinear(guide.rgb) * asfloat(P[2].z)));
    }
    guideOut[id] = guide;
    if (P[3].y != UNX_NONE)
    {
        // (no history: luma 0, gradient 0 - its code is 127 / 255 -, no variation)
        float4 flicker = float4(0, 127.0 / 255.0, 0, 0);
        if (!offScreen && P[3].x != UNX_NONE)
        {
            Texture2D<float4> previousFlicker = ResourceDescriptorHeap[P[3].x];
            uint3 h = uint3(id, P[3].z & 7u) * uint3(1664525u, 22695477u, 2891336453u) + 1013904223u;
            h.x += h.y * h.z;
            h.y += h.z * h.x;
            h ^= h >> 16;
            const float2 e = float2(h.xy & 0xFFFFu) / 65536.0;
            const int2 at = clamp(int2(floor(previousUv * float2(size) + e - 0.5)), 0, size - 1);
            flicker = previousFlicker.Load(int3(at, 0));
            // the luma's exposure (the guide space, as a grey)
            const float linearLuma = flicker.r * min(0.17 / max(1.0 - flicker.r, 1e-6), 65504.0) * asfloat(P[2].z);
            flicker.r = saturate(linearLuma / (linearLuma + 0.17));
        }
        RWTexture2D<float4> flickerOut = ResourceDescriptorHeap[P[3].y];
        flickerOut[id] = flicker;
    }
    maskOut[id] = float2(((offScreen ? 1.0 : 0.0) + (disoccluded ? 2.0 : 0.0)) / 255.0, edge);
}
