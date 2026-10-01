// unx-kernel: cs_6_6 main
// Turbid basin water as a froxel medium (defect queue 13 (75), FEATURES_GAME 1.10; shading.water_turbid): every W2 basin
// whose Water material scatters (sigma_s > 0) adds, per froxel (tile, slice) of the main view, the optical depth of the
// tile-centre ray's segment inside the basin's box and the single-scattered source along it, in the layout S's
// integration takes from E's particle media (VolumeSlices.hlsl: (tile, s) = tau, (tile, S + s) = the self-attenuated
// source in nits before exposure); when E's media are present this frame the two are combined exactly
// (raw sources add, the self-attenuation is of the sum). The integration then mixes water and air uniformly in the
// slice, and the refracted view through the surface sees the slices' in-scatter along the straight tile ray (the
// design's condition: bend x depth / tile width <= 1 tile at bath distances).
// Source at the segment (midpoint lighting, length len):
//   sun:    E_sun(x) x W's sun map transmittance (refracted direction, Fresnel, absorption to the surface, caustics;
//           WaterLight.hlsli waterSunLight) x the VSM air walk's lit fraction of the segment x p_HG(nu, g) sigma_s len;
//   locals: the froxel's list, FroxelSlice.hlsli airLocalLight with coefficients (extinction sigma_t, rayleigh 0,
//           mie sigma_s) and the material's g (unshadowed in the water, v1: the lamps above a bath cast no shadow into
//           it but the surface's; the surface's own refraction of the lamp is left out, Fresnel (1 - F) ~ 0.98);
//   ambient: the GI cache's irradiance from above and below, isotropic ((E_up + E_down) / 2 pi) sigma_s len.
// P[0] = { froxel lights SRV (raw, FroxelGrid header), volume slices UAV (RWTexture3D<float4>), basin count, basins SRV
//        (raw, 64 B each: centre xyz, cos yaw, sin yaw, half x, half z, depth (0: 1e4), sigma_s rgb, g, sigma_a rgb, 0) }
// P[1] = { W's sun map depth, normal, medium, constants SRVs (UNX_NONE: no sun map this frame: no sun term) }
// P[2] = { caustics SRV (UNX_NONE: none), GI cache SRV (UNX_NONE: none), light functions SRV (UNX_NONE: none),
//        existing media (1: the slices hold E's media: combine; 0: write every texel) }
// P[3] = { VSM page table SRV, atlas SRV, blocks SRV, VSM constants CBV (UNX_NONE: the sun unshadowed) }
// P[4] = { VSM search bound SRV, shadow texels per tile (float bits), transmittance LUT, multi-scatter LUT (UNX_NONE: E
//        at the top of the atmosphere) }
// Frame constants of the main view.
#include "Passes/Atmosphere/FroxelSlice.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Water/WaterLight.hlsli"
#include "Passes/GI/GiCache.hlsli"

// Clips [za, zb] (ray parameters) to the slab lo <= o + d t <= hi.
void waterSlab(float o, float d, float lo, float hi, inout float za, inout float zb)
{
    if (abs(d) < 1e-9)
    {
        if (o < lo || o > hi) zb = za - 1;
        return;
    }
    float t0 = (lo - o) / d, t1 = (hi - o) / d;
    if (t0 > t1)
    {
        const float t = t0;
        t0 = t1;
        t1 = t;
    }
    za = max(za, t0);
    zb = min(zb, t1);
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    const FroxelGrid g = froxelGrid(P[0].x);
    const uint2 tile = gid.xy;
    if (tile.x >= g.gridX || tile.y >= g.gridY || s >= g.slices) return;
    RWTexture3D<float4> slices = ResourceDescriptorHeap[P[0].y];
    const float3 ray = froxelTileRay(g, tile);  // camera-relative, unit view depth
    const float speed = length(ray);
    const float3 dir = ray / speed;
    const float zs0 = froxelNodeDepth(g, s), zs1 = froxelNodeDepth(g, s + 1);
    ByteAddressBuffer basins = ResourceDescriptorHeap[P[0].w];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
    FroxelSrvs f;
    f.lights = P[0].x;
    f.lightIndices = P[0].x;
    f.scattering = UNX_NONE;
    f.pad = 0;
    const uint indexBase = froxelIndexBase(f);
    const uint2 range = lists.Load2(g.headerBase + froxelIndex(g, tile, s) * 8);
    const float3 sunDir = normalize(g_sunDirection);
    float3 tauW = 0, rawW = 0;
    for (uint b = 0; b < P[0].z; ++b)
    {
        const uint o = b * 64;
        const float3 centre = asfloat(basins.Load3(o));
        const float cy = asfloat(basins.Load(o + 12)), sy = asfloat(basins.Load(o + 16));
        const float hx = asfloat(basins.Load(o + 20)), hz = asfloat(basins.Load(o + 24)), depth = asfloat(basins.Load(o + 28));
        const float3 sigmaS = asfloat(basins.Load3(o + 32));
        const float gw = asfloat(basins.Load(o + 44));
        const float3 sigmaA = asfloat(basins.Load3(o + 48));
        // the tile-centre ray in the basin's frame (Pool.cpp axes: local x = dx cos - dz sin, local z = dx sin + dz cos)
        const float3 d0 = g_cameraPosition - centre;
        const float3 lo = float3(d0.x * cy - d0.z * sy, d0.y, d0.x * sy + d0.z * cy);
        const float3 ld = float3(ray.x * cy - ray.z * sy, ray.y, ray.x * sy + ray.z * cy);
        float za = zs0, zb = zs1;
        waterSlab(lo.x, ld.x, -hx, hx, za, zb);
        waterSlab(lo.y, ld.y, -(depth > 0 ? depth : 1e4), 0, za, zb);
        waterSlab(lo.z, ld.z, -hz, hz, za, zb);
        if (!(zb > za)) continue;
        const float len = (zb - za) * speed, zm = 0.5 * (za + zb);
        const float3 a = g_cameraPosition + ray * za, pm = g_cameraPosition + ray * zm;
        tauW += (sigmaA + sigmaS) * len;
        AirCoefficients c;
        c.extinction = sigmaA + sigmaS;
        c.rayleigh = 0;
        c.mie = sigmaS;
        float3 src = 0;
        // sun through the surface
        if (P[1].x != UNX_NONE)
        {
            float3 lw, tw;
            if (waterSunLight(P[1].x, P[1].y, P[1].z, P[1].w, P[2].x, pm, sunDir, 0, lw, tw))
            {
                float3 E = g_sunIlluminance * g_sunColor;
                if (P[4].z != UNX_NONE)
                {
                    AtmosphereSrvs atm;
                    atm.transmittance = P[4].z;
                    atm.multiScatter = P[4].w;
                    atm.skyView = UNX_NONE;
                    atm.aerial = UNX_NONE;
                    E = atmosphereSunIlluminance(atm, pm);
                }
                float lit = 1;
                if (P[3].w != UNX_NONE)
                {
                    VsmResources r;
                    r.table = ResourceDescriptorHeap[P[3].x];
                    r.pool = ResourceDescriptorHeap[P[3].y];
                    r.blocks = ResourceDescriptorHeap[P[3].z];
                    r.searchBound = ResourceDescriptorHeap[P[4].x];
                    r.cbv = P[3].w;
                    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[3].w];
                    uint k;
                    if (vsmAirLevel(vc, froxelTileWidth(g, zm), asfloat(P[4].y), k)) lit = 1 - vsmAirShadowFraction(r, a, a + dir * len, k);
                }
                src += E * tw * lit * airMiePhase(dot(dir, lw), gw) * sigmaS * len;
            }
        }
        // local lights of the froxel's list
        for (uint i = 0; i < range.y; ++i)
        {
            const uint li = froxelLightAt(f, indexBase, range.x + i);
            const GpuLight l = loadLight(li);
            if (airLocalMap(l, a, dir, len).h >= l.range) continue;
            src += airLocalLight(l, a, dir, len, c, gw, P[2].z, li, froxelTileWidth(g, zm));
        }
        // ambient from the GI cache
        if (P[2].y != UNX_NONE)
        {
            GiSrvs gi;
            gi.cache = P[2].y;
            gi.hash = P[2].y;
            gi.pad0 = gi.pad1 = 0;
            const float3 L = (giCacheIrradiance(gi, pm, float3(0, 1, 0)) + giCacheIrradiance(gi, pm, float3(0, -1, 0))) / (2 * GI_PI);
            src += sigmaS * len * L;
        }
        rawW += src;
    }
    float3 tau = tauW, raw = rawW;
    if (P[2].w != 0)
    {
        const float3 tauP = slices[uint3(tile, s)].rgb, sP = slices[uint3(tile, g.slices + s)].rgb;
        raw += sP / froxelSelfAttenuation(tauP);  // E's raw source (VolumeSlices: S_p = S_raw (1 - e^-tau) / tau)
        tau += tauP;
    }
    else if (!any(tauW > 0))
    {
        slices[uint3(tile, s)] = 0;
        slices[uint3(tile, g.slices + s)] = 0;
        return;
    }
    if (!all(isfinite(tau)) || !all(isfinite(raw)))
    {
        tau = 0;
        raw = 0;
    }
    slices[uint3(tile, s)] = float4(tau, 0);
    slices[uint3(tile, g.slices + s)] = float4(min(raw * froxelSelfAttenuation(tau), 65504.0), 0);
}
