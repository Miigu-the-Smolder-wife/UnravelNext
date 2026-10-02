// unx-kernel: cs_6_6 main
// s.fog.scatter (FogVolume.hlsli): one thread per cell of the fog's volume. The cell's extinction and the light it
// scatters toward the camera per metre, at a point jittered inside the cell each frame (P[7]), blended with the cell's
// history - the previous frame's volume read at the cell centre's place in the previous view.
//   sun        E x the air's transmittance to the sun x HG(view . sun, g) x (1 - the shadowed fraction of the cell's
//              segment of its centre ray): the casters' shadow from the sun's shadow pages at the air level of the
//              cell's width (VsmMarkFog.hlsl asked for exactly these pages, at the level of the unjittered cell). The
//              segment stays in front of the surface on the centre ray (the centre pixel's depth): a segment that
//              would cross it is moved toward the camera by what lies behind (fogSegment; past the surface the ray is
//              in another space - beyond a wall, outside);
//   local      the air grid's sampled local light (MegaLightsVolume.hlsl: visible fluence and its direction moment per
//              froxel, shadow rays for every caster), read between its froxels, through the phase function's first two
//              SH bands;
//   indirect   the Lumen translucency volume of the previous frame (this frame's is built after the fog).
// A cell wholly behind the farthest surface of its pixels is not computed (extinction -1): the integration passes
// through it, and a history lookup that touches such a cell is dropped (a disoccluded cell starts from this frame).
// P[0] = { grid x | y << 16, z | cell px << 16, asuint(far m), asuint(k) }
// P[1] = { asuint(b), scatter UAV (Texture3D RGBA16F: rgb nits / m, a 1 / m), history SRV (UNX_NONE: none), flags }
// P[2] = asuint{ density (1/m at the fog's height), height falloff, height (m), phase g }
// P[3] = asuint{ albedo r, g, b, start distance (m) }
// P[4] = { VSM table SRV, atlas SRV, blocks SRV, constants CBV (UNX_NONE: no sun shadows) }
// P[5] = { VSM search bound SRV, asuint(shadow texels per cell), air lists SRV (the froxel grid; UNX_NONE: no local
//          light), depth pyramid SRV }
// P[6] = { local fluence SRV, direction moment SRV, previous translucency volume params SRV (UNX_NONE: none),
//          transmittance LUT SRV }
// P[7] = asuint{ jitter x, y, z in [0, 1), history weight }, P[8] = { VSM stats UAV (the walk's error word), depth SRV,
//          asuint(the density's noise amount), asuint(1 / its scale in m) }, P[9].xyz = asuint(the noise's lattice offset)
// Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(P[0], P[1].x);
    if (any(id >= uint3(g.x, g.y, g.z))) return;
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[1].y];
    // the farthest surface of the cell's pixels
    Texture2D<float> hiz = ResourceDescriptorHeap[P[5].w];
    const float farthest = g_nearPlane / max(hiz.Load(int3(id.xy, fogHizMip(g))), 1e-30);
    const float z0 = fogDepthOfSlice(g, float(id.z)), z1 = fogDepthOfSlice(g, float(id.z) + 1.0);
    if (z0 >= farthest)
    {
        output[id] = float4(0, 0, 0, -1);  // (extinction -1: not computed - the integration takes 0, a history lookup that meets it is dropped)
        return;
    }
    const float3 jitter = asfloat(P[7].xyz);
    const float2 centrePixel = (float2(id.xy) + 0.5) * float(g.cellPx), samplePixel = (float2(id.xy) + jitter.xy) * float(g.cellPx);
    const float3 centreRay = froxelRayAt(centrePixel);
    const float3 ray = froxelRayAt(samplePixel);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    // the surfaces on the two rays the cell walks (a pixel outside the view: the view's edge pixel)
    Texture2D<float> depthTexture = ResourceDescriptorHeap[P[8].y];
    const int2 last = int2(g_viewWidth, g_viewHeight) - 1;
    const float centreDepth = linearDepth(depthTexture.Load(int3(min(int2(centrePixel), last), 0)));
    const float sampleDepth = linearDepth(depthTexture.Load(int3(min(int2(samplePixel), last), 0)));
    // the sample point: at the frame's place in the cell, in front of the surface on its own ray
    const float zs = max(min(fogDepthOfSlice(g, float(id.z) + jitter.z), sampleDepth - 0.02), 0.0);
    const float3 p = g_cameraPosition + ray * zs;

    const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3]);
    float sigma = fogExtinctionAt(fog, p.y) * fogDensityScale(p * float3(1, 2, 1) * asfloat(P[8].w) + asfloat(P[9].xyz), asfloat(P[8].z));
    if (zs * toRay < fog.start) sigma = 0;
    float3 inScattered = 0;
    if (sigma > 0)
    {
        // the sun
        const float3 E = g_sunIlluminance * g_sunColor;
        if (any(E > 0))
        {
            const float3 sun = normalize(g_sunDirection);
            const AtmosphereParams a = airParamsFromTexels(P[6].w);
            float shadowed = 0;
            if (P[4].w != 0xFFFFFFFFu)
            {
                VsmResources r;
                r.table = ResourceDescriptorHeap[P[4].x];
                r.pool = ResourceDescriptorHeap[P[4].y];
                r.blocks = ResourceDescriptorHeap[P[4].z];
                r.searchBound = ResourceDescriptorHeap[P[5].x];
                r.cbv = P[4].w;
                ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[4].w];
                uint k;
                // (the level of the unjittered cell: the pages VsmMarkFog.hlsl asked for)
                if (vsmAirLevel(vc, fogCellWidth(g, 0.5 * (z0 + z1)), asfloat(P[5].y), k))
                {
                    float za = max(fogDepthOfSlice(g, float(id.z) + jitter.z - 0.5), 0.0), zb = fogDepthOfSlice(g, float(id.z) + jitter.z + 0.5);
                    fogSegment(za, zb, (P[1].w & 1u) != 0 ? min(g.farM, centreDepth) : g.farM);
                    if (zb > za + 1e-4)
                    {
                        VsmAirWalkCount walk = (VsmAirWalkCount)0;
                        shadowed = vsmAirShadowFraction(r, g_cameraPosition + centreRay * za, g_cameraPosition + centreRay * zb, k, walk);
                        if (walk.capped != 0 && P[8].x != 0xFFFFFFFFu)
                        {
                            RWByteAddressBuffer stats = ResourceDescriptorHeap[P[8].x];
                            stats.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_AIR_WALK);
                        }
                    }
                }
            }
            inScattered += E * airSunTransmittance(a, P[6].w, airLiftToSurface(a, p), sun) * ((1 - saturate(shadowed)) * airMiePhase(dot(dir, sun), fog.g));
        }
        // the local lights
        if (P[5].z != 0xFFFFFFFFu && P[6].x != 0xFFFFFFFFu)
        {
            const FroxelGrid ag = froxelGrid(P[5].z);
            Texture3D<float4> fluenceVolume = ResourceDescriptorHeap[P[6].x];
            Texture3D<float4> momentVolume = ResourceDescriptorHeap[P[6].y];
            const float2 pixel = (float2(id.xy) + jitter.xy) * float(g.cellPx);
            const float3 uvw = float3(pixel / float2(ag.gridX * ag.tilePx, ag.gridY * ag.tilePx),
                                      clamp(froxelSliceCoord(ag, zs) / float(ag.slices), 0.5 / float(ag.slices), 1.0 - 0.5 / float(ag.slices)));
            const float3 F = fluenceVolume.SampleLevel(g_linearClamp, uvw, 0).rgb / g_exposure;
            const float3 M = momentVolume.SampleLevel(g_linearClamp, uvw, 0).rgb / g_exposure;
            const float lumF = dot(F, float3(0.2126, 0.7152, 0.0722));
            if (lumF > 0) inScattered += F * (max(0.0, 1.0 + 3.0 * fog.g * dot(M, dir) / lumF) / (4.0 * 3.14159265358979));
        }
        // the indirect light
        if (P[6].z != 0xFFFFFFFFu) inScattered += ltvInscatter(P[6].z, p, dir, fog.g);
    }
    float4 value = float4(fog.albedo * inScattered * sigma, sigma);
    if (any(isnan(value)) || any(isinf(value))) value = 0;

    // history: the cell's centre in the previous view
    if (P[1].z != 0xFFFFFFFFu)
    {
        const float zc = min(fogDepthOfSlice(g, float(id.z) + 0.5), max(farthest - 0.02, 0.0));
        const float3 centre = g_cameraPosition + centreRay * zc;
        const float4 clip = mul(g_prevViewProj, float4(centre, 1));
        if (clip.w > 1e-4)
        {
            const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            const float slice = fogSliceOfDepth(g, clip.w);
            if (all(uv > 0) && all(uv < 1) && slice < float(g.z))
            {
                Texture3D<float4> history = ResourceDescriptorHeap[P[1].z];
                const float2 scale = float2(g_viewWidth, g_viewHeight) / float2(g.x * g.cellPx, g.y * g.cellPx);
                const float4 h = history.SampleLevel(g_linearClamp, float3(uv * scale, clamp(slice / float(g.z), 0.5 / float(g.z), 1.0 - 0.5 / float(g.z))), 0);
                if (!any(isnan(h)) && h.a >= 0) value = lerp(value, h, asfloat(P[7].w));  // (a < 0: cells that were hidden)
            }
        }
    }
    output[id] = value;
}
