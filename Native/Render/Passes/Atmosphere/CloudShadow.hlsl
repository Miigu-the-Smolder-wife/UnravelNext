// unx-kernel: cs_6_6 main
// Cloud sun transmittance map (B5; S_STATUS_KO.md 9): a deep opacity map in the sun's orthographic space, stored by
// optical-depth levels. Texel (i, j) is the sun ray through the plane point shadowCentre + (u, v) (u, v in [-e, e], e =
// half extent, the plane perpendicular to the sun direction); marching it down from the layer's top in
// CLOUD_SHADOW_STEP midpoint steps, the texel keeps the altitude where the optical depth from the top first reaches each
// level tau_k (cloudShadowLevel: 0.05 .. 8, CLOUD_SHADOW_LEVELS of them; unorm16 over [base, top], 0 = never reached)
// and, in the last slot, the whole layer's optical depth (fp16). Transmittance then has its resolution where it changes: an edge where tau rises over one mean
// free path (~25 m) gets its levels there, which equally spaced altitude knots (156 m) blurred [measured, CloudTests:
// radiance 26 % mean error with 16 altitude knots against 1.0 % with the sun path integrated exactly].
// P[0] = { cloud record SRV (raw), shadow UAV, 0, 0 }; one thread per map texel.
#include "Passes/Atmosphere/CloudCommon.hlsli"
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const CloudRecord c = cloudLoad(P[0].x);
    const uint n = (uint)c.shadowTexels;
    if (any(id >= n)) return;
    RWTexture2D<uint4> map = ResourceDescriptorHeap[P[0].y];
    float3 U, V;
    cloudShadowBasis(c.sunDir, U, V);
    const float2 uv = ((float2(id) + 0.5) / n * 2 - 1) * c.shadowHalfExtent;
    const float3 through = c.shadowCentre + U * uv.x + V * uv.y;
    const float3 o = through + c.sunDir * CLOUD_SHADOW_RAY_BACK, d = -c.sunDir;
    const float tTop = cloudShadowAltitudeDistance(c, o, d, c.top), tBase = cloudShadowAltitudeDistance(c, o, d, c.base);
    float level[CLOUD_SHADOW_LEVELS];
    [unroll] for (uint k = 0; k < CLOUD_SHADOW_LEVELS; ++k) level[k] = c.base;
    float tau = 0;
    uint next = 0;
    if (tTop < tBase && tBase < 3.0e38)
    {
        const uint steps = min((uint)ceil((tBase - tTop) / CLOUD_SHADOW_STEP), CLOUD_SHADOW_MAX_STEPS);
        const float dt = (tBase - tTop) / steps;
        [loop] for (uint s = 0; s < steps; ++s)
        {
            const float3 x = o + d * (tTop + (s + 0.5) * dt);
            const float rho = cloudDensity(c, x);
            if (rho <= 0) continue;
            const float before = tau;
            tau += rho * dt;
            // Levels crossed inside this step: the crossing altitude interpolated linearly in tau over the step.
            const float aStart = cloudAltitude(c, o + d * (tTop + s * dt)), aEnd = cloudAltitude(c, o + d * (tTop + (s + 1) * dt));
            [loop] while (next < CLOUD_SHADOW_LEVELS && tau >= cloudShadowLevel(next))
            {
                level[next] = lerp(aStart, aEnd, (cloudShadowLevel(next) - before) / (tau - before));
                ++next;
            }
        }
    }
    uint slot[16];
    [unroll] for (uint k = 0; k < CLOUD_SHADOW_LEVELS; ++k) slot[k] = cloudShadowPackAltitude(c, level[k], k < next);
    slot[15] = f32tof16(tau);  // the whole layer's optical depth along this ray
    [unroll] for (uint q = 0; q < 2; ++q)
    {
        uint4 v;
        [unroll] for (uint e = 0; e < 4; ++e) v[e] = slot[8 * q + 2 * e] | (slot[8 * q + 2 * e + 1] << 16);
        map[uint2(id.x * 2 + q, id.y)] = v;
    }
}
