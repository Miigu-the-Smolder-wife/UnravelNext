// unx-kernel: cs_6_6 main
// Round basins (RoundPool.hlsli): one group per source adds its Gaussian footprint w (sigma >= the ring spacing and the
// wall's angular spacing) to the polar accumulation, over the angles within 4 sigma of the centre and the rings within
// 4 sigma radially, normalised by its own discrete sum S = sum w A (A = r h_r h_theta the sample's area): the added field
// integrates to exactly the footprint's. A footprint sample beyond the wall (r > R) folds radially onto 2 R - r (the
// source's image in the tangent plane of the nearest wall point: exact for a straight wall, error O(sigma / R) for the
// curvature); the wall ring itself counts once (its sample sits on the wall).
//   impulse I: phi changes by -(I / rho) w / S;  volume V: eta changes by -V w / S (the mean-level term V / (pi R^2) is
//   the host's sum in double, RoundRows adds P[4].w).
// 2^24 fixed point (order independent).
#include "Passes/Water/RoundPool.hlsli"

groupshared float g_sum[256];

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    if (g >= P[2].y) return;
    ByteAddressBuffer sources = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    const float4 s = asfloat(sources.Load4(32 * g));
    const float volume = asfloat(sources.Load(32 * g + 16));
    const float R = roundRadius(), hr = roundRingSpacing(), hTheta = 2 * OCEAN_PI / float(ROUND_THETA);
    const float rs = length(s.xy), thetaS = atan2(s.y, s.x);
    const float sigma = max(s.z, max(hr, R * hTheta));
    const float reach = 4.0 * sigma;
    // rings within reach (the folded image of rings past the wall lands on rings inside: the radial range stays [0, R])
    const int j0 = max(int(floor((rs - reach) / hr)) - 1, 0), j1 = min(int(ceil((rs + reach) / hr)) + 1, int(ROUND_RINGS) - 1 + int(ROUND_MAX_REACH));
    // angles within reach at the nearest ring (the widest span): half-angle asin(reach / r), the whole circle when wider
    const float rNear = max(rs - reach, hr);
    const float halfAngle = reach >= rNear ? OCEAN_PI : asin(saturate(reach / rNear));
    const int iHalf = min(int(ceil(halfAngle / hTheta)) + 1, int(ROUND_THETA) / 2);
    const int iCentre = int(round(thetaS / hTheta));
    const int widthI = 2 * iHalf + 1, widthJ = j1 - j0 + 1, count = widthI * widthJ;
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);
    float partial = 0;
    for (int q = int(t); q < count; q += 256)
    {
        const int ju = j0 + q / widthI, iu = iCentre - iHalf + q % widthI;  // unfolded ring (may lie past the wall)
        const float r = roundRingRadius(uint(max(ju, 0)));
        const float2 p = r * float2(cos(float(iu) * hTheta), sin(float(iu) * hTheta));
        const float2 d = p - s.xy;
        partial += exp(-dot(d, d) * inv2s2) * (r * hr * hTheta);
    }
    g_sum[t] = partial;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (t < stride) g_sum[t] += g_sum[t + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    const float amplitude = -(s.w / ROUND_RHO) / g_sum[0], lowering = -volume / g_sum[0];
    for (int q2 = int(t); q2 < count; q2 += 256)
    {
        const int ju = j0 + q2 / widthI, iu = iCentre - iHalf + q2 % widthI;
        if (ju < 0) continue;
        const float r = roundRingRadius(uint(ju));
        const float2 p = r * float2(cos(float(iu) * hTheta), sin(float(iu) * hTheta));
        const float2 d = p - s.xy;
        const float w = exp(-dot(d, d) * inv2s2);
        int jf = ju;
        if (jf >= int(ROUND_RINGS)) jf = 2 * (int(ROUND_RINGS) - 1) - jf;  // past the wall: the radial fold (r -> 2 R - r)
        if (jf < 0) continue;
        const uint i = uint((iu % int(ROUND_THETA) + int(ROUND_THETA)) % int(ROUND_THETA));
        const uint at = 8 * (i + uint(jf) * ROUND_THETA);
        if (volume != 0) accum.InterlockedAdd(at, int(round(lowering * w * ROUND_FIXED)));
        if (s.w != 0) accum.InterlockedAdd(at + 4, int(round(amplitude * w * ROUND_FIXED)));
    }
}
