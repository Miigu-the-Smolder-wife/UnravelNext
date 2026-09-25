// unx-kernel: cs_6_6 main
// Froxel integration (ARCHITECTURE 2.3 "볼륨 산란", INTERFACES 5.6 froxelScattering). One group per screen tile, one
// thread per depth slice; slice s is the segment of the tile-centre ray between nodes s and s + 1 (slice 0 starts at
// the camera, node S = farM). Per slice, with the air's coefficients at the segment's midpoint (constant along it):
//  - sun: the single-scattered sunlight the air does NOT receive, -f x E_sun T_sun (sigma_R P_R + sigma_M P_M)
//    (1 - e^(-sigma_t L)) / sigma_t, where f is the segment's shadowed fraction by length (vsmAirShadowFraction at the
//    froxel's lateral resolution). The multiple-scattering term of the air perspective is not reduced (its sources are
//    the whole sky, not the sun direction);
//  - local lights of the froxel's list: in-scattering by the air, int e^(-sigma_t t) (sigma_R P_R + sigma_M P_M) I(w)
//    window(d) / d^2 dt, integrated in the angle subtended at the light (t = t_c + h tan theta, dt / d^2 = dtheta / h:
//    the integrand is smooth in theta) with 8-point Gauss-Legendre. Area lights act as point sources of their
//    projected intensity (froxelIntensity). No local-light shadows in the air yet (S_STATUS_KO.md);
//  - optical depth sigma_t L.
// Then a group scan: node n = sum over slices j < n of e^(-tau before j) x source_j, written to volume slice n - 1 as
// rgb x exposure of the main view (fp16 keeps the relative precision of what is displayed, also at night exposures where
// local lights' air glow is 1e-4 nits; froxelScattering divides it out), with a = 1 (no local media in scenes v1).
// P[0].x froxelLights SRV (raw), P[0].y volume UAV (RWTexture3D<float4>), P[0].z transmittance LUT, P[0].w multi-scatter LUT
// P[1].x VSM page table SRV (raw), .y pool SRV (raw), .z blocks SRV (raw), .w VSM constants CBV (0xFFFFFFFF: no VSM)
// P[2].x VSM search bound SRV (raw), P[2].y shadow texels per tile (float bits)
// Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"

groupshared float3 gs_tau[64];
groupshared float3 gs_source[64];

static const float kGaussX[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                  0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363 };
static const float kGaussW[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                  0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763 };

// Air in-scattering of light l along o + dir t, t in [0, len] (nits), relative to the segment's start.
float3 airLocalLight(GpuLight l, float3 o, float3 dir, float len, AirCoefficients c, float mieG)
{
    const float tc = dot(l.position - o, dir);
    // Distance of the line from the light, not below the emitter's size (1 cm for points): the point-source integrand
    // is singular on the line.
    const float h = max(length(l.position - (o + dir * tc)), max(max(l.size.x, l.size.y), 0.01));
    const float th0 = atan(-tc / h), th1 = atan((len - tc) / h);
    const float mid = 0.5 * (th0 + th1), half = 0.5 * (th1 - th0);
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        const float th = mid + half * kGaussX[i];
        const float t = tc + h * tan(th);
        const float3 v = o + dir * t - l.position;
        const float d = h / cos(th);
        const float3 w = v / max(length(v), 1e-6);
        const float nu = -sin(th);  // cosine between the light's propagation (w) and the path to the camera (-dir)
        const float3 phase = c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, mieG);
        sum += kGaussW[i] * froxelIntensity(l, w) * froxelWindow(l, d) * phase * exp(-c.extinction * max(t, 0.0));
    }
    return sum * (half / h) * l.color;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    const FroxelGrid g = froxelGrid(P[0].x);
    const uint2 tile = gid.xy;
    const AtmosphereParams a = airParamsFromTexels(P[0].w);
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    float3 tau = 0, source = 0;
    if (s < g.slices)
    {
        const float z0 = froxelNodeDepth(g, s), z1 = froxelNodeDepth(g, s + 1);
        const float t0 = z0 * toRay, len = (z1 - z0) * toRay;
        const float3 o = g_cameraPosition + dir * t0;
        const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
        const AirCoefficients c = airCoefficients(a, max(0.0, airAltitude(a, pm)));
        tau = c.extinction * len;
        // Sun.
        if (P[1].w != 0xFFFFFFFFu)
        {
            VsmResources r;
            r.table = ResourceDescriptorHeap[P[1].x];
            r.pool = ResourceDescriptorHeap[P[1].y];
            r.blocks = ResourceDescriptorHeap[P[1].z];
            r.searchBound = ResourceDescriptorHeap[P[2].x];
            r.cbv = P[1].w;
            ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[1].w];
            const float3 sun = normalize(g_sunDirection);
            const float3 sunT = airSunTransmittance(a, P[0].z, pm, sun);
            if (any(sunT > 0))
            {
                const uint k = vsmAirLevel(vc, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[2].y));
                const float f = vsmAirShadowFraction(r, o, o + dir * len, k);
                const float nu = dot(dir, sun);
                const float3 phase = c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, a.mieG);
                source -= f * (g_sunIlluminance * g_sunColor) * sunT * phase * airIntegral(c.extinction, len);
            }
        }
        // Local lights of the froxel's list.
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4);
        const uint first = h >> 6, count = h & 63u;
        for (uint i = 0; i < count; ++i)
        {
            const uint w = lists.Load(g.indexBase + ((first + i) >> 1) * 4);
            const uint li = ((first + i) & 1) ? w >> 16 : w & 0xFFFFu;
            source += airLocalLight(loadLight(li), o, dir, len, c, a.mieG);
        }
    }
    // Exclusive scan of the optical depth, inclusive scan of the attenuated sources (Hillis-Steele over 64 slices).
    gs_tau[s] = tau;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint d = 1; d < 64; d <<= 1)
    {
        const float3 add = s >= d ? gs_tau[s - d] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_tau[s] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    gs_source[s] = exp(-(gs_tau[s] - tau)) * source;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint d2 = 1; d2 < 64; d2 <<= 1)
    {
        const float3 add = s >= d2 ? gs_source[s - d2] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_source[s] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (s < g.slices)
    {
        RWTexture3D<float4> volume = ResourceDescriptorHeap[P[0].y];
        volume[uint3(tile, s)] = float4(gs_source[s] * g_exposure, 1);  // pre-exposed: fp16 precision follows the display
    }
}
