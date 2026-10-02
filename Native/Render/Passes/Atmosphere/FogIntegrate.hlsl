// unx-kernel: cs_6_6 main
// s.fog.integrate (FogVolume.hlsli): one thread per column of the fog's volume, front to back. Per slice the light
// scattered inside it reaches the camera through what lies in front: with extinction s and source S (per metre) over a
// length d, the slice adds T x S (1 - e^(-s d)) / s and T becomes T e^(-s d) (the integral of a homogeneous slab: no
// energy is lost to the slice's own thickness). Stored per slice: rgb = radiance in-scattered between the camera and
// the slice's far face (nits), a = transmittance to that face.
// The column's far source (2D, rgb nits per unit of optical depth): what the fog beyond the volume scatters toward the
// camera - the sun through the phase function without casters (the shadow pages are not asked for out there), and the
// indirect light at the volume's end. FogApply.hlsl multiplies it by the closed-form opacity of the fog past farM.
// P[0] = { grid x | y << 16, z | cell px << 16, asuint(far m), asuint(k) }
// P[1] = { asuint(b), scatter SRV, integrated UAV (Texture3D RGBA16F), far source UAV (Texture2D RGBA16F) }
// P[2] = asuint{ density, height falloff, height, phase g }, P[3] = asuint{ albedo r, g, b, start distance }
// P[6] = { -, -, previous translucency volume params SRV (UNX_NONE: none), transmittance LUT SRV }
// Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(P[0], P[1].x);
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
    const float3 p = g_cameraPosition + ray * g.farM;
    float3 inScattered = 0;
    const float3 E = g_sunIlluminance * g_sunColor;
    if (any(E > 0))
    {
        const float3 sun = normalize(g_sunDirection);
        const AtmosphereParams a = airParamsFromTexels(P[6].w);
        inScattered += E * airSunTransmittance(a, P[6].w, airLiftToSurface(a, p), sun) * airMiePhase(dot(dir, sun), fog.g);
    }
    if (P[6].z != 0xFFFFFFFFu) inScattered += ltvInscatter(P[6].z, p, dir, fog.g);
    float3 farSource = fog.albedo * inScattered;
    if (any(isnan(farSource)) || any(isinf(farSource))) farSource = 0;
    RWTexture2D<float4> farOut = ResourceDescriptorHeap[P[1].w];
    farOut[id.xy] = float4(farSource, 1);
}
