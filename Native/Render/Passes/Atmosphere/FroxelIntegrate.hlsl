// unx-kernel: cs_6_6 main
// Air volume of the main view on the froxel grid (ARCHITECTURE 2.3 "볼륨 산란"; Atmosphere.hlsli atmosphereAerial /
// atmosphereAirView). One group per screen tile, one thread per depth slice; slice s is the segment of the tile-centre
// ray between nodes s and s + 1 (node 0 = camera, node S = far_m). Per slice, midpoint substeps of at most
// air_step_altitude_m altitude change (exact exponential within each):
//  - the atmosphere's single scattering with the tile-centre phase, times (1 - f), f the segment's fraction shadowed by
//    casters (vsmAirShadowFraction, at shadow_texels_per_tile texels per tile width); multiple scattering (Hillaire's
//    Psi_ms; its sources are the whole sky and are not reduced by the casters' shadows);
//  - local lights of the froxel's list: in-scattering by the air, int e^(-sigma_t t) (sigma_R P_R + sigma_M P_M) I(w)
//    window(d) / d^2 dt, integrated in the angle subtended at the light (t = t_c + h tan theta, dt / d^2 = dtheta / h:
//    the integrand is smooth in theta) with 8-point Gauss-Legendre. Area lights act as point sources of their
//    projected intensity (froxelIntensity). No local-light shadows in the air yet (S_STATUS_KO.md);
//  - optical depth.
// A group scan gives node n = sum over slices j < n of e^(-tau before j) x source_j. Volume (RGBA16F, gridX x gridY x
// 3 (S + 1)): part 0 in-scattering x exposure of the main view (fp16 keeps the relative precision of what is displayed,
// also at night exposures where local lights' air glow is 1e-4 nits), part 1 optical depth, part 2 sun transmittance at
// the node. No local media in scenes v1 (their optical depth adds to part 1).
// P[0].x froxelLights SRV (raw), P[0].y volume UAV (RWTexture3D<float4>), P[0].z transmittance LUT, P[0].w multi-scatter LUT
// P[1].x VSM page table SRV (raw), .y pool SRV (raw), .z blocks SRV (raw), .w VSM constants CBV (0xFFFFFFFF: no VSM)
// P[2].x VSM search bound SRV (raw), P[2].y shadow texels per tile (float bits), P[2].z air step altitude m (float bits),
// P[2].w experiment mask (atmosphere.froxels.experiment_disable; 0; cost attribution only: 1 air shadows, 2 local lights,
// 4 air integration, 8 sun transmittance per substep, 16 multiple scattering per substep)
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
    const uint tlut = P[0].z, mlut = P[0].w;
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float3 sun = normalize(g_sunDirection);
    const float3 E = g_sunIlluminance * g_sunColor;
    const float nu = dot(dir, sun);
    const float phaseR = airRayleighPhase(nu), phaseM = airMiePhase(nu, a.mieG);
    const float stepAltitude = asfloat(P[2].z);
    const uint experiment = P[2].w;
    float3 tau = 0, source = 0;
    if (s < g.slices)
    {
        const float z0 = froxelNodeDepth(g, s), z1 = froxelNodeDepth(g, s + 1);
        const float t0 = z0 * toRay, len = (z1 - z0) * toRay;
        const float3 o = g_cameraPosition + dir * t0;
        // Substeps: the air's density is exponential in altitude; midpoint steps of at most stepAltitude.
        const float h0 = airAltitude(a, o), h1 = airAltitude(a, o + dir * len), hm = airAltitude(a, o + dir * (0.5 * len));
        const float dh = max(max(abs(h1 - h0), abs(hm - h0)), abs(hm - h1));
        const uint steps = (experiment & 4) ? 0u : (uint)clamp(ceil(dh / stepAltitude), 1.0, 32.0);
        const float dt = len / steps;
        float3 single = 0, multi = 0;
        [loop] for (uint k = 0; k < steps; ++k)
        {
            const float3 p = airLiftToSurface(a, o + dir * ((k + 0.5) * dt));
            const AirCoefficients c = airCoefficients(a, max(0.0, airAltitude(a, p)));
            const float3 w = exp(-tau) * airIntegral(c.extinction, dt);
            single += w * (c.rayleigh * phaseR + c.mie * phaseM) * ((experiment & 8) ? 1.0 : airSunTransmittance(a, tlut, p, sun));
            multi += w * (c.rayleigh + c.mie) * ((experiment & 16) ? 1.0 : airMultipleScattering(a, mlut, p, sun));
            tau += c.extinction * dt;
        }
        // Casters' shadows in the air: the shadowed fraction of the segment removes that part of the single scattering.
        float f = 0;
        if (P[1].w != 0xFFFFFFFFu && any(single > 0) && (experiment & 1) == 0)
        {
            VsmResources r;
            r.table = ResourceDescriptorHeap[P[1].x];
            r.pool = ResourceDescriptorHeap[P[1].y];
            r.blocks = ResourceDescriptorHeap[P[1].z];
            r.searchBound = ResourceDescriptorHeap[P[2].x];
            r.cbv = P[1].w;
            ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[1].w];
            uint k;
            if (vsmAirLevel(vc, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[2].y), k)) f = vsmAirShadowFraction(r, o, o + dir * len, k);
        }
        source = E * (single * (1 - f) + multi);
        // Local lights of the froxel's list (air at the segment's midpoint).
        const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
        const AirCoefficients cm = airCoefficients(a, max(0.0, airAltitude(a, pm)));
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4);
        const uint first = h >> 6, count = (experiment & 2) ? 0u : h & 63u;
        for (uint i = 0; i < count; ++i)
        {
            const uint w = lists.Load(g.indexBase + ((first + i) >> 1) * 4);
            const uint li = (((first + i) & 1) ? w >> 16 : w & 0xFFFFu) & 0x7FFFu;
            source += airLocalLight(loadLight(li), o, dir, len, cm, a.mieG);
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
        // Node s + 1 of the three parts; thread 0 also writes node 0 (the camera).
        RWTexture3D<float4> volume = ResourceDescriptorHeap[P[0].y];
        const uint N = g.slices + 1;
        const float3 node = g_cameraPosition + dir * (froxelNodeDepth(g, s + 1) * toRay);
        volume[uint3(tile, s + 1)] = float4(min(gs_source[s] * g_exposure, 65504.0), 0);  // pre-exposed: fp16 precision follows the display
        volume[uint3(tile, N + s + 1)] = float4(gs_tau[s], 0);
        volume[uint3(tile, 2 * N + s + 1)] = float4(airSunTransmittance(a, tlut, airLiftToSurface(a, node), sun), 0);
        if (s == 0)
        {
            volume[uint3(tile, 0)] = 0;
            volume[uint3(tile, N)] = 0;
            volume[uint3(tile, 2 * N)] = float4(airSunTransmittance(a, tlut, airLiftToSurface(a, g_cameraPosition), sun), 0);
        }
    }
}
