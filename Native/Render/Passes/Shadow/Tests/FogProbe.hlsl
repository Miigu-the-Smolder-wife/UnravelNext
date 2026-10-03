// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5,6
// FogTests: the fog's functions (Fog.hlsli, FogVolume.hlsli) at query points, one thread per query; FogTests.cpp holds
// their C++ twins.
// MODE 0: the closed form. 3 float4 per query: { ray origin, t0 }, { ray direction (unit), t1 }, { a height y, 0, 0, 0 };
//         writes { fogOpticalDepth(origin, direction, t0, t1), fogExtinctionAt(y), 0, 0 }. The medium: P[2] = asuint{ density,
//         height falloff, height, phase g }, P[3] = asuint{ albedo, start distance } (fogMedium's words).
// MODE 1: the grid mapping. 1 float4 per query { x, za, zb, limit }; writes 2 float4: { fogSliceOfDepth(x), fogDepthOfSlice(x),
//         fogFarDepth(x), fogDepthOfSlice(fogSliceOfDepth(x)) } and fogSegment(za, zb, limit) as { za, zb, 0, 0 }. The grid:
//         P[2] = fogGrid's first word, P[3] = { asuint(b), asuint(far end m), 0, 0 }.
// MODE 2: the density's noise. { lattice coordinate, amount } -> { fogDensityScale, fogValueNoise, fogLatticeValue at the
//         coordinate's floor, 0 }.
// MODE 3: the readers, with frame constants that carry the fog's record. 3 float4 per query: { uv, view depth, ray length },
//         { ray origin, 0 }, { ray direction (unit), 0 }; writes 6 float4: fogAt(uv, depth); fogVolumeAt of the record's volume
//         and grid at the same place; fogOverAir on in-scattering (1, 2, 3) nits and transmittance (0.5, 0.6, 0.7): the
//         in-scattering, then the transmittance; fogOverRay over that ray on radiance (100, 200, 300) nits; fogOverSky(uv) on
//         the same radiance, with the record's sky amount in w (-1: the view has no volume).
// MODE 4: the sun at a point as FogScatter.hlsl takes it. { position, 0 } -> { the air's transmittance to the sun x the
//         cloud layer's, 1 }. P[2].x = the transmittance LUT's SRV.
// MODE 5: the view's rays. { pixel position x, y, 0, 0 } -> { froxelRayAt(pixel), 0 } (FroxelCommon.hlsli: the ray through
//         the pixel position at unit view depth).
// P[0] = { queries SRV (StructuredBuffer<float4>), output UAV (RWStructuredBuffer<float4>), query count, 0 }.
// Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[0].z) return;
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].y];
#if MODE == 0
    const float4 q0 = queries[3 * id], q1 = queries[3 * id + 1], q2 = queries[3 * id + 2];
    const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3]);
    output[id] = float4(fogOpticalDepth(fog, q0.xyz, q1.xyz, q0.w, q1.w), fogExtinctionAt(fog, q2.x), 0, 0);
#elif MODE == 6
    const float4 q0 = queries[3 * id], q1 = queries[3 * id + 1], q2 = queries[3 * id + 2];
    FogMedium fog = (FogMedium)0;
    fog.on = true; fog.density2 = q2.y; fog.falloff2 = -1; fog.height2 = q2.x;
    output[id] = float4(fogOpticalDepth(fog, q0.xyz, q1.xyz, q0.w, q1.w), fogExtinctionAt(fog, q2.z), fogSunThrough(fog, q0.xyz, q1.xyz), 0);
#elif MODE == 1
    const float4 q = queries[id];
    const FogGrid g = fogGrid(P[2], P[3].x, P[3].y);
    output[2 * id] = float4(fogSliceOfDepth(g, q.x), fogDepthOfSlice(g, q.x), fogFarDepth(g, q.x), fogDepthOfSlice(g, fogSliceOfDepth(g, q.x)));
    float za = q.y, zb = q.z;
    fogSegment(za, zb, q.w);
    output[2 * id + 1] = float4(za, zb, 0, 0);
#elif MODE == 2
    const float4 q = queries[id];
    output[id] = float4(fogDensityScale(q.xyz, q.w), fogValueNoise(q.xyz), fogLatticeValue(int3(floor(q.xyz))), 0);
#elif MODE == 3
    const float4 q0 = queries[3 * id], q1 = queries[3 * id + 1], q2 = queries[3 * id + 2];
    output[6 * id] = fogAt(q0.xy, q0.z);
    FogParams p;
    const bool there = fogLoad(p);
    float4 fromVolume = float4(0, 0, 0, 1);
    if (there)
    {
        Texture3D<float4> integrated = ResourceDescriptorHeap[p.volumeSrv];
        const FogGrid g = fogGrid(uint4(p.grid & 0x7FFFFFFFu, p.slices, asuint(p.farM), asuint(p.k)), asuint(p.b), asuint(p.farEndM));
        fromVolume = fogVolumeAt(integrated, g, q0.xy, q0.z);
    }
    output[6 * id + 1] = fromVolume;
    float3 inscatter = float3(1, 2, 3), transmittance = float3(0.5, 0.6, 0.7);
    fogOverAir(q0.xy, q0.z, inscatter, transmittance);
    output[6 * id + 2] = float4(inscatter, 0);
    output[6 * id + 3] = float4(transmittance, 0);
    const float3 radiance = float3(100, 200, 300);
    output[6 * id + 4] = float4(fogOverRay(q0.xy, q0.z, q1.xyz, q2.xyz, q0.w, radiance), 0);
    output[6 * id + 5] = float4(fogOverSky(q0.xy, radiance), there ? p.skyAmount : -1.0);
#elif MODE == 4
    const float3 p = queries[id].xyz;
    const AtmosphereParams a = airParamsFromTexels(P[2].x);
    output[id] = float4(airSunTransmittance(a, P[2].x, airLiftToSurface(a, p), normalize(g_sunDirection)) * cloudSunTransmittanceFromLut(P[2].x, p), 1);
#else
    output[id] = float4(froxelRayAt(queries[id].xy), 0);
#endif
}
