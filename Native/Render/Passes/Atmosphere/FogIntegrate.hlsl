// unx-kernel: cs_6_6 main
// s.fog.integrate (FogVolume.hlsli): one thread per column of the fog's volume, front to back. Per slice the light
// scattered inside it reaches the camera through what lies in front: with extinction s and source S (per metre) over a
// length d, the slice adds T x S (1 - e^(-s d)) / s and T becomes T e^(-s d) (the integral of a homogeneous slab: no
// energy is lost to the slice's own thickness). Stored per slice: rgb = radiance in-scattered between the camera and
// the slice's far face (nits), a = transmittance to that face.
// Past the volume's end the integration goes on through zFar slices (fogFarDepth) with the exponential height fog in
// closed form (Fog.hlsli fogOpticalDepth) and the column's far source (nits per unit of optical depth): the sun through
// the phase function, outside the casters' shadow as the air volume found it for its own slices there (the fraction of
// each froxel slice's segment in shadow: FroxelIntegrate.hlsl, part 2's alpha - atmosphere.fog.far_shadows), and the
// indirect light at farM.
// P[0] = { grid x | y << 16, z | cell px << 16 | zFar << 24, asuint(far m), asuint(k) }
// P[1] = { asuint(b), scatter SRV, integrated UAV (Texture3D RGBA16F, z + zFar slices), asuint(far end m) }
// P[2] = asuint{ density, height falloff, height, phase g }, P[3] = asuint{ albedo r, g, b, start distance }
// P[6] = { air volume SRV (this frame's; UNX_NONE: the far fog without casters), froxel lights SRV (the air's grid),
//          previous translucency volume params SRV (UNX_NONE: none), transmittance LUT SRV }
// Frame constants of the view (the main view, or a planar reflection view).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

// The part of a stretch of the column's ray (view depths za < zb) outside the casters' shadow, from the air volume: slice
// s's shadowed fraction is at node s + 1 of part 2. Two taps in log depth, each between the two nodes around it and
// across the tiles (the parts are not blended into: whole nodes are fetched).
float fogFarLit(Texture3D<float4> air, FroxelGrid fg, float2 pixel, float za, float zb)
{
    uint w, h, d;
    air.GetDimensions(w, h, d);
    const float2 uv = pixel / (float2(w, h) * float(fg.tilePx));
    const float base = 2.0 * float(fg.slices + 1) + 0.5;
    float shadowed = 0;
    for (uint i = 0; i < 2; ++i)
    {
        const float depth = za * pow(zb / za, 0.25 + 0.5 * float(i));
        const float n = clamp(froxelSliceCoord(fg, depth) + 0.5, 1.0, float(fg.slices));
        const float n0 = floor(n), n1 = min(n0 + 1.0, float(fg.slices));
        shadowed += 0.5 * lerp(air.SampleLevel(g_linearClamp, float3(uv, (base + n0) / float(d)), 0).a,
                               air.SampleLevel(g_linearClamp, float3(uv, (base + n1) / float(d)), 0).a, n - n0);
    }
    return 1.0 - saturate(shadowed);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(P[0], P[1].x, P[1].w);
    if (id.x >= g.x || id.y >= g.y) return;
    Texture3D<float4> scatter = ResourceDescriptorHeap[P[1].y];
    RWTexture3D<float4> integrated = ResourceDescriptorHeap[P[1].z];
    const float3 ray = froxelRayAt((float2(id.xy) + 0.5) * float(g.cellPx));
    const float toRay = length(ray);
    float3 L = 0;
    float T = 1;
    float before = 0;
    [loop] for (uint z = 0; z < g.z; ++z)
    {
        const float4 s = scatter.Load(int4(id.xy, z, 0));
        const float after = fogDepthOfSlice(g, float(z) + 1.0);
        const float d = (after - before) * toRay;
        before = after;
        const float sigma = max(s.a, 0.0);  // (-1: a cell that was not computed)
        const float t = exp(-sigma * d);
        const float3 source = s.a > 0 ? max(s.rgb, 0.0) : float3(0, 0, 0);
        L += T * (sigma > 1e-7 ? source * ((1 - t) / sigma) : source * d);
        T *= t;
        integrated[uint3(id.xy, z)] = float4(L, T);
    }
    // the fog beyond the volume
    const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3]);
    const float3 dir = ray / toRay;
    const float tStart = airViewStart(g_clipPlane, g_cameraPosition, dir);  // (a planar reflection view: from the mirror on)
    const float3 p = g_cameraPosition + dir * max(g.farM * toRay, tStart);
    float3 fromSun = 0, fromAround = 0;
    const float3 E = g_sunIlluminance * g_sunColor;
    if (any(E > 0))
    {
        const float3 sun = normalize(g_sunDirection);
        const AtmosphereParams a = airParamsFromTexels(P[6].w);
        fromSun = fog.albedo * E * airSunTransmittance(a, P[6].w, airLiftToSurface(a, p), sun) * airMiePhase(dot(dir, sun), fog.g);
    }
    if (P[6].z != 0xFFFFFFFFu) fromAround = fog.albedo * ltvInscatter(P[6].z, p, dir, fog.g);
    if (any(isnan(fromSun)) || any(isinf(fromSun))) fromSun = 0;
    if (any(isnan(fromAround)) || any(isinf(fromAround))) fromAround = 0;
    const bool farShadows = P[6].x != 0xFFFFFFFFu && any(fromSun > 0);
    FroxelGrid fg = (FroxelGrid)0;
    if (farShadows) fg = froxelGrid(P[6].y);
    const float2 pixel = (float2(id.xy) + 0.5) * float(g.cellPx);
    [loop] for (uint i = 0; i < g.zFar; ++i)
    {
        const float za = fogFarDepth(g, float(i)), zb = fogFarDepth(g, float(i) + 1.0);
        const float t = exp(-fogOpticalDepth(fog, g_cameraPosition, dir, max(za * toRay, tStart), zb * toRay));
        float3 farSource = fromSun + fromAround;
        if (farShadows)
        {
            Texture3D<float4> air = ResourceDescriptorHeap[P[6].x];
            farSource = fromSun * fogFarLit(air, fg, pixel, za, zb) + fromAround;
        }
        L += T * farSource * (1 - t);
        T *= t;
        integrated[uint3(id.xy, g.z + i)] = float4(L, T);
    }
}
