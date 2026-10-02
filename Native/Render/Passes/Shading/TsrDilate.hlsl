// unx-kernel: cs_6_6 main
// m.tsr.dilate (Tsr.hlsli; the reference's TSRDilateVelocity): per internal pixel, the closest depth of its 3 x 3
// neighbourhood and that pixel's reprojection vector (an edge pixel takes the foreground's vector), the depth error of
// the neighbourhood (how much depth varies across a pixel there: the parallax test's allowance), the reprojection edge
// (1: the dilated vector is the pixel's own, 0: a pixel or more apart), and the scatter: the pixel written, with its
// previous device depth, to the four pixels around where it was in the previous frame (atomic max: the closest surface
// that was there, among this frame's surfaces).
// The reprojection field (P[2].y bit 0; output.upscale_tsr_reprojection_field):
//   jacobian   the vector's change per pixel along x and y, from the pixel's own (undilated) vector and its neighbours'
//              under the depth weights of the depth error: a neighbour across a depth edge has no weight, so the
//              jacobian is the pixel's own surface's (a turning or approaching surface). Where neither axis has a
//              neighbour on the surface, the diagonals, turned back to the axes;
//   boundary   where the dilated vector differs from the pixel's own by more than P[2].z (squared, twice the input
//              pixels - the reference's unit), the depth edge between the pixel and the neighbour across it is followed
//              3 pixels both ways as m.tsr.aa follows a luma edge, and the edge's line through the pixel is stored as
//              the vector from it to the pixel's corner deepest in the foreground (Tsr.hlsli tsrInsideBoundary).
//              Elsewhere the whole pixel is the foreground's.
// P[0] = { depth SRV (the surface the vectors are of: the view's depth, or the tracked depth of UpscaleMotion.hlsl),
//          motion SRV (RG32F, output-UV offsets; UpscaleMotion.hlsl), previous depth SRV (RG32F: view depth,
//          is-moving), dilated motion UAV (RG32F) }
// P[1] = { info UAV (RGBA16F: previous closest device depth, device depth error, reprojection edge, a = (1: the vector
//          came from a neighbour) + is-moving / 2), scatter UAV (R32_UINT, cleared), width, height }
// P[2] = { field UAV (RGBA32_UINT: jacobian, offset and boundary, the closest device depth's bits, 0; UNX_NONE: none),
//          flags (1: the reprojection field's jacobian and boundary; without it the field holds a zero jacobian, the
//          whole pixel and the closest depth - what the history resurrection reprojects by), asuint(boundary
//          threshold), 0 }. Frame constants b1 = the main view.
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

#define BOUNDARY_ITERATIONS 3

// The device depth error of one pixel's size in the world at that depth (the depth of a point two pixel radii further).
float pixelDeviceZError(float deviceZ)
{
    if (!(deviceZ > 0)) return 0;
    const float depth = linearDepth(deviceZ);
    const float radius = depth * g_tanHalfFovY / g_viewHeight;
    return abs(g_nearPlane / (depth + 2.0 * radius) - deviceZ);
}

// The depth weights of a pixel's four neighbours along two axes (the reference's ComputeDepthBilaterals): each side's
// depth step, weighted down where it is an edge rather than a slope; xy = the first axis' forward and backward
// neighbour, zw = the second's, each pair normalised. error: the depth step the weights leave, the larger of the axes.
// False: no neighbour on the pixel's surface along either axis.
bool depthBilaterals(float c, float e, float w, float s, float n, float pixelDistance, out float4 weights, out float error)
{
    const float variationEW = abs(c - (e + w) * 0.5), variationSN = abs(c - (s + n) * 0.5);
    const float diffE = abs(e - c), diffW = abs(w - c), diffS = abs(s - c), diffN = abs(n - c);
    const float minE = min(abs(e - w), min(diffE, diffW)), minS = min(abs(s - n), min(diffS, diffN));
    const float pixelError = max(pixelDeviceZError(c) * pixelDistance, 1e-12);
    const float finalE = min(max(minE, pixelError), max(minS, 8.0 * pixelError));
    const float finalS = min(max(minS, pixelError), max(minE, 8.0 * pixelError));
    const float we = saturate(1.5 - min(diffE, variationEW) / finalE), ww = saturate(1.5 - min(diffW, variationEW) / finalE);
    const float ws = saturate(1.5 - min(diffS, variationSN) / finalS), wn = saturate(1.5 - min(diffN, variationSN) / finalS);
    const float totalEW = we + ww, totalSN = ws + wn;
    const float invEW = totalEW > 0 ? 1.0 / totalEW : 0.0, invSN = totalSN > 0 ? 1.0 / totalSN : 0.0;
    weights = float4(we * invEW, ww * invEW, ws * invSN, wn * invSN);
    error = max(diffE * weights.x + diffW * weights.y, diffS * weights.z + diffN * weights.w);
    return totalEW > 0 || totalSN > 0;
}

// The boundary of a pixel whose depth edge was followed lengthP / lengthN pixels along +- the browse direction until it
// ended by rising (the foreground takes the pixel's row there) or by falling (the reference's ComputeReprojectionBoundary:
// the foreground's reach into the pixel is half a pixel, less where the edge steps towards it, more where it steps away).
float2 reprojectionBoundary(int2 side, float lengthP, float lengthN, bool incrementP, bool incrementN, bool decrementP, bool decrementN)
{
    const float invMaxLength = 1.0 / (float)(BOUNDARY_ITERATIONS + 1);
    const float invLength = max(1.0 / (1.0 + lengthN + lengthP), invMaxLength);
    float maxDistance = 1.0, maxInclination = 0.0;
    if (decrementP && decrementN)
    {
        maxDistance = (min(lengthP, lengthN) + 0.5) * invMaxLength;
        maxInclination = lengthP < lengthN ? -invMaxLength : (lengthN < lengthP ? invMaxLength : 0.0);
    }
    else if (decrementP)
    {
        maxDistance = (lengthP + 0.5) * invLength;
        maxInclination = -invLength;
    }
    else if (decrementN)
    {
        maxDistance = (lengthN + 0.5) * invLength;
        maxInclination = invLength;
    }
    float minDistance = 0.0, minInclination = 0.0;
    if (incrementP && incrementN)
    {
        minDistance = 1.0 - (min(lengthP, lengthN) + 0.5) * invMaxLength;
        minInclination = lengthP < lengthN ? invMaxLength : (lengthN < lengthP ? -invMaxLength : 0.0);
    }
    else if (incrementP)
    {
        minDistance = 1.0 - (lengthP + 0.5) * invLength;
        minInclination = invLength;
    }
    else if (incrementN)
    {
        minDistance = 1.0 - (lengthN + 0.5) * invLength;
        minInclination = -invLength;
    }
    minDistance = max(minDistance, 1.0 / 1023.0);
    // half a pixel of dilation, inside what the edge's ends allow
    const float dilate = 0.5;
    const float distanceToEdge = max(minDistance, min(maxDistance, dilate));
    float inclination = 0.0;
    if (maxDistance <= dilate) inclination = maxInclination;
    if (minDistance > dilate) inclination = minInclination;
    // the edge's line: through the pixel at distanceToEdge from the side's border, its normal the side tilted by the
    // inclination along the browse direction; the boundary = from the line to the corner deepest on the normal's side
    const float2 fSide = float2(side), along = abs(fSide.yx);
    const float2 normal = normalize(fSide + along * inclination);
    const float2 corner = saturate(sign(normal));
    const float2 onEdge = float2(0.5, 0.5) + fSide * (0.5 - distanceToEdge);
    return dot(corner - onEdge, normal) * normal;
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
    const bool field = (P[2].y & 1u) != 0 && P[2].x != UNX_NONE;

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
    // The depth error along the two axes: (z: 0 NW, 1 N, 2 NE, 3 W, 4 the pixel, 5 E, 6 SW, 7 S, 8 SE)
    float4 weights;
    float depthError;
    const bool onSurface = depthBilaterals(z[4], z[5], z[3], z[7], z[1], 1.0, weights, depthError);
    const int2 from = clamp(int2(id) + offset, 0, size - 1);
    const float2 own = motion.Load(int3(id, 0)), vector = motion.Load(int3(from, 0));
    const float2 previousSample = previousDepth.Load(int3(from, 0));
    const float previousView = previousSample.x;
    const float previousZ = previousView > 0 ? g_nearPlane / previousView : 0.0;
    const float2 deltaPixels = (vector - own) * float2(size);
    const float edge = saturate(1.1 - dot(abs(deltaPixels), float2(1.1, 1.1)));

    if (field)
    {
        // the jacobian of the pixel's own vector (input pixels per input pixel)
#define VELOCITY(x, y) (motion.Load(int3(clamp(int2(id) + int2(x, y), 0, size - 1), 0)) * float2(size))
        const float2 vC = own * float2(size);
        float2 dx = 0, dy = 0;
        bool valid = all(abs(own) < 1.5);  // (a point behind the previous camera has no vector: UpscaleMotion's (2, 2))
        if (onSurface)
        {
            const float2 vE = VELOCITY(1, 0), vW = VELOCITY(-1, 0), vS = VELOCITY(0, 1), vN = VELOCITY(0, -1);
            dx = (vE - vC) * weights.x + (vC - vW) * weights.y;
            dy = (vS - vC) * weights.z + (vC - vN) * weights.w;
            valid = valid && all(abs(float4(vE, vW)) < 1.5 * float4(size, size)) && all(abs(float4(vS, vN)) < 1.5 * float4(size, size));
        }
        else
        {
            // the diagonals (first axis towards SE, second towards SW), turned back to x and y
            float4 diagonal;
            if (depthBilaterals(z[4], z[8], z[0], z[6], z[2], 1.4142136, diagonal, depthError))
            {
                const float2 vSE = VELOCITY(1, 1), vNW = VELOCITY(-1, -1), vSW = VELOCITY(-1, 1), vNE = VELOCITY(1, -1);
                const float2 dSE = (vSE - vC) * diagonal.x + (vC - vNW) * diagonal.y;
                const float2 dSW = (vSW - vC) * diagonal.z + (vC - vNE) * diagonal.w;
                dx = 0.5 * dSE - 0.5 * dSW;
                dy = 0.5 * dSE + 0.5 * dSW;
                valid = valid && all(abs(float4(vSE, vNW)) < 1.5 * float4(size, size)) && all(abs(float4(vSW, vNE)) < 1.5 * float4(size, size));
            }
        }
#undef VELOCITY
        if (!valid || !all(isfinite(float4(dx, dy))))
        {
            dx = 0;
            dy = 0;
        }

        // the boundary of the dilation
        float2 boundary = float2(0, 1);  // the whole pixel
        const float2 previousUv = (float2(id) + 0.5) / float2(size) - vector;
        const float2 differential = 2.0 * deltaPixels;
        if (any(offset != 0) && dot(differential, differential) > asfloat(P[2].z) && all(previousUv >= 0) && all(previousUv <= 1))
        {
            // the edge through the pixel: along x or y by the neighbourhood's variation, on the side of the larger step
            const float c = z[4], n = z[1], s = z[7], e = z[5], w = z[3];
            const float variationH = abs(0.5 * (z[2] + z[0]) - n) + abs(0.5 * (e + w) - c) + abs(0.5 * (z[8] + z[6]) - s);
            const float variationV = abs(0.5 * (z[2] + z[8]) - e) + abs(0.5 * (n + s) - c) + abs(0.5 * (z[0] + z[6]) - w);
            const bool vertical = variationH > variationV;
            const int2 browse = vertical ? int2(0, 1) : int2(1, 0);
            int2 side = 0;
            float edgeZ;
            if (vertical)
            {
                side.x = abs(w - c) > abs(e - c) ? -1 : 1;
                edgeZ = side.x < 0 ? w : e;
            }
            else
            {
                side.y = abs(n - c) > abs(s - c) ? -1 : 1;
                edgeZ = side.y < 0 ? n : s;
            }
            // followed both ways halfway between the pixel's row and the edge's: the mean of the two rows' depths leaves
            // the band around their mean when one of the rows steps to the other's side
            const float merged = 0.5 * (edgeZ + c), band = 0.25 * abs(edgeZ - c);
            bool minP = false, maxP = false, minN = false, maxN = false;
            float lengthP = BOUNDARY_ITERATIONS, lengthN = BOUNDARY_ITERATIONS;
            [unroll] for (int i = 1; i <= BOUNDARY_ITERATIONS; ++i)
            {
                const int2 p = clamp(int2(id) + browse * i, 0, size - 1), q = clamp(int2(id) - browse * i, 0, size - 1);
                const float sampleP = 0.5 * (depth.Load(int3(p, 0)) + depth.Load(int3(clamp(p + side, 0, size - 1), 0)));
                const float sampleN = 0.5 * (depth.Load(int3(q, 0)) + depth.Load(int3(clamp(q + side, 0, size - 1), 0)));
                const bool stopMinP = sampleP < merged - band && !maxP, stopMaxP = sampleP > merged + band && !minP;
                const bool stopMinN = sampleN < merged - band && !maxN, stopMaxN = sampleN > merged + band && !minN;
                minP = minP || stopMinP;
                maxP = maxP || stopMaxP;
                minN = minN || stopMinN;
                maxN = maxN || stopMaxN;
                lengthP -= (minP || maxP) ? 1.0 : 0.0;
                lengthN -= (minN || maxN) ? 1.0 : 0.0;
            }
            const bool closer = edgeZ > c;  // (the side's row is the foreground)
            boundary = reprojectionBoundary(side, lengthP, lengthN, closer ? maxP : minP, closer ? maxN : minN, closer ? minP : maxP, closer ? minN : maxN);
        }
        RWTexture2D<uint4> fieldOut = ResourceDescriptorHeap[P[2].x];
        fieldOut[id] = uint4(tsrEncodeJacobian(dx, dy), tsrEncodeBoundary(offset, boundary), asuint(closest), 0);
    }
    else if (P[2].x != UNX_NONE)
    {
        RWTexture2D<uint4> fieldOut = ResourceDescriptorHeap[P[2].x];
        fieldOut[id] = uint4(tsrEncodeJacobian(float2(0, 0), float2(0, 0)), tsrEncodeBoundary(offset, float2(0, 1)), asuint(closest), 0);
    }
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
