// unx-kernel: cs_6_6 main
// m.tsr.dilate (Tsr.hlsli; the reference's TSRDilateVelocity): per internal pixel, the closest depth of its 3 x 3
// neighbourhood and that pixel's reprojection vector (an edge pixel takes the foreground's vector), the depth error of
// the neighbourhood (how much depth varies across a pixel there: the parallax test's allowance), the reprojection edge
// (1: the dilated vector is the pixel's own, 0: a pixel or more apart), and the scatter: the pixel written, with its
// previous device depth, to the four pixels around where it was in the previous frame (atomic max: the closest surface
// that was there, among this frame's surfaces).
// P[0] = { depth SRV, motion SRV (RG32F, output-UV offsets; UpscaleMotion.hlsl), previous depth SRV (RG32F: view depth,
//          is-moving), dilated motion UAV (RG32F) }
// P[1] = { info UAV (RGBA16F: previous closest device depth, device depth error, reprojection edge, a = (1: the vector
//          came from a neighbour) + is-moving / 2), scatter UAV (R32_UINT, cleared), width, height }. Frame constants
// b1 = the main view.
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

// The device depth error of one pixel's size in the world at that depth (the depth of a point two pixel radii further).
float pixelDeviceZError(float deviceZ)
{
    if (!(deviceZ > 0)) return 0;
    const float depth = linearDepth(deviceZ);
    const float radius = depth * g_tanHalfFovY / g_viewHeight;
    return abs(g_nearPlane / (depth + 2.0 * radius) - deviceZ);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 size = int2(P[1].zw);
    if (any(int2(id) >= size)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<float2> motion = ResourceDescriptorHeap[P[0].y];
    Texture2D<float2> previousDepth = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float2> dilated = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float4> info = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<uint> scatter = ResourceDescriptorHeap[P[1].y];

    float z[9];
    float closest = -1;
    int2 offset = 0;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const int2 o = int2(k % 3, k / 3) - 1;
        z[k] = depth.Load(int3(clamp(int2(id) + o, 0, size - 1), 0));
        // (the centre first among equals: reversed Z, the largest is the closest)
        if (z[k] > closest || (z[k] == closest && k == 4))
        {
            closest = z[k];
            offset = o;
        }
    }
    // The depth error along the two axes (the reference's ComputeDepthBilaterals): each side's depth step, weighted down
    // where it is an edge rather than a slope, the larger of the axes.
    float depthError;
    {
        const float c = z[4], e = z[5], w = z[3], s = z[7], n = z[1];
        const float variationEW = abs(c - (e + w) * 0.5), variationSN = abs(c - (s + n) * 0.5);
        const float diffE = abs(e - c), diffW = abs(w - c), diffS = abs(s - c), diffN = abs(n - c);
        const float minE = min(abs(e - w), min(diffE, diffW)), minS = min(abs(s - n), min(diffS, diffN));
        const float pixelError = max(pixelDeviceZError(c), 1e-12);
        const float finalE = min(max(minE, pixelError), max(minS, 8.0 * pixelError));
        const float finalS = min(max(minS, pixelError), max(minE, 8.0 * pixelError));
        float we = saturate(1.5 - min(diffE, variationEW) / finalE), ww = saturate(1.5 - min(diffW, variationEW) / finalE);
        float ws = saturate(1.5 - min(diffS, variationSN) / finalS), wn = saturate(1.5 - min(diffN, variationSN) / finalS);
        const float totalEW = we + ww, totalSN = ws + wn;
        const float invEW = totalEW > 0 ? 1.0 / totalEW : 0.0, invSN = totalSN > 0 ? 1.0 / totalSN : 0.0;
        depthError = max((diffE * we + diffW * ww) * invEW, (diffS * ws + diffN * wn) * invSN);
    }
    const int2 from = clamp(int2(id) + offset, 0, size - 1);
    const float2 own = motion.Load(int3(id, 0)), vector = motion.Load(int3(from, 0));
    const float2 previousSample = previousDepth.Load(int3(from, 0));
    const float previousView = previousSample.x;
    const float previousZ = previousView > 0 ? g_nearPlane / previousView : 0.0;
    const float2 deltaPixels = (vector - own) * float2(size);
    const float edge = saturate(1.1 - dot(abs(deltaPixels), float2(1.1, 1.1)));
    dilated[id] = vector;
    info[id] = float4(previousZ, depthError, edge, (any(offset != 0) ? 1.0 : 0.0) + 0.5 * saturate(previousSample.y));

    // scatter to the previous position (in this frame's pixel grid)
    const float2 velocityPixels = vector * float2(size);
    const float2 previousPixel = float2(id) + 0.5 - velocityPixels;
    if (all(previousPixel > 0) && all(previousPixel < float2(size)) && all(abs(vector) < 1.5))
    {
        const uint value = (f32tof16(previousZ) << TSR_HOLE_BITS) | tsrEncodeHoleVelocity(velocityPixels);
        const TsrBilinear b = tsrBilinear(previousPixel);
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const int2 texel = tsrBilinearTexel(b, i);
            if (round(tsrBilinearWeight(b, i) * 63.0) > 0 && all(texel >= 0) && all(texel < size)) InterlockedMax(scatter[texel], value);
        }
    }
}
