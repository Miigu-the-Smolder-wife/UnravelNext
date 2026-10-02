// unx-kernel: cs_6_6 main
// s.fog.scatter (FogVolume.hlsli): one thread per cell of the fog's volume. The cell's extinction and the light it
// scatters toward the camera per metre, at a point jittered inside the cell each frame (P[7]), blended with the cell's
// history - the previous frame's volume read at the cell centre's place in the previous view. The light is stored x the
// view's exposure; the history's is brought to this frame's exposure first (P[10].y).
//   sun        E x the air's transmittance to the sun x the cloud layer's (the sun map at the sample point) x
//              HG(view . sun, g) x (1 - the shadowed fraction of the cell's
//              segment of its centre ray): the casters' shadow from the sun's shadow pages at the air level of the
//              cell's width (VsmMarkFog.hlsl asked for exactly these pages, at the level of the unjittered cell). The
//              segment stays in front of the surface on the centre ray (the centre pixel's depth): a segment that
//              would cross it is moved toward the camera by what lies behind (fogSegment; past the surface the ray is
//              in another space - beyond a wall, outside); x what E's grooms let through from the sample point towards
//              the sun (Passes/Hair/HairDensity.hlsli hairTransmittance; P[10].z);
//   local      the air grid's sampled local light (MegaLightsVolume.hlsl: visible fluence and its direction moment per
//              froxel, shadow rays for every caster), read between its froxels, through the phase function's first two
//              SH bands;
//   indirect   the Lumen translucency volume of the previous frame (this frame's is built after the fog).
// A cell wholly behind the farthest surface of its pixels is not computed (extinction -1): the integration passes
// through it, and a history lookup that touches such a cell is dropped (a disoccluded cell starts from this frame).
// P[0] = { grid x | y << 16, z | cell px << 16, asuint(far m), asuint(k) }
// P[1] = { asuint(b), scatter UAV (Texture3D RGBA16F: rgb nits / m x g_exposure, a 1 / m), history SRV (UNX_NONE: none),
//          flags }
// P[2] = asuint{ density (1/m at the fog's height), height falloff, height (m), phase g }
// P[3] = asuint{ albedo r, g, b, start distance (m) }
// P[4] = { VSM table SRV, atlas SRV, blocks SRV, constants CBV (UNX_NONE: no sun shadows) }
// P[5] = { VSM search bound SRV, asuint(shadow texels per cell), air lists SRV (the froxel grid; UNX_NONE: no local
//          light), depth pyramid SRV (UNX_NONE: every cell is computed) }
// P[6] = { local fluence SRV, direction moment SRV, previous translucency volume params SRV (UNX_NONE: none),
//          transmittance LUT SRV }
// P[7] = asuint{ jitter x, y, z in [0, 1), history weight }, P[8] = { VSM stats UAV (the walk's error word), depth SRV,
//          asuint(the density's noise amount), asuint(1 / its scale in m) }, P[9].xyz = asuint(the noise's lattice offset)
// P[9].w = the local volumes' records SRV (raw, 96 B each: the rows of unit-from-render - the unit sphere or the cube
//          [-1, 1]^3 -, then { density, height falloff, 1 / edge, albedo r | g << 8 | b << 16 | shape << 24 }, { the
//          turbulence's lattice per unit of the volume's axes xyz, its amount }, { its rise (lattice), the source plane's
//          height in the volume [0, 1), the density grid's first byte (P[10].w's buffer), its size x | y << 8 | z << 16
//          (0: none) }), P[10].x = their count (FrameContext::fogVolumes). A volume adds density x fade toward its
//          boundary x 2^(-falloff x the height above its source plane, 0 .. 1; nothing under the plane) x the density's
//          variation x its own turbulence (FogVolume.hlsli fogSteamScale: rising steam) x its grid's value, with its own
//          albedo; the cell's light is the same.
// P[10].w = the frame's density grids (raw SRV; UNX_NONE: no volume has one).
// P[11] = asuint{ the medium's second layer: density (1/m at its height), height falloff, height (m), 0 } (Fog.hlsli)
// P[10].y = asuint(this frame's exposure / the history's: the history's light at this frame's exposure; 1 without history)
// P[10].z = E's hair density parameters (raw SRV; UNX_NONE: none - no hair this frame, or shading.hair_shadows off).
// P[11].w = FX's particle shadow map parameters (raw SRV; UNX_NONE: none; Passes/FX/ParticleShadow.hlsli): the sun's
//           light in a cell times what the shadow-casting sprites above it let through.
// Frame constants of the view (the main view, or a planar reflection view: its fog starts at the mirror).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"
#include "Passes/Hair/HairDensity.hlsli"
#include "Passes/FX/ParticleShadow.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(P[0], P[1].x);
    if (any(id >= uint3(g.x, g.y, g.z))) return;
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[1].y];
    // the farthest surface of the cell's pixels
    float farthest = 3.0e38;
    if (P[5].w != 0xFFFFFFFFu)
    {
        Texture2D<float> hiz = ResourceDescriptorHeap[P[5].w];
        farthest = g_nearPlane / max(hiz.Load(int3(id.xy, fogHizMip(g))), 1e-30);
    }
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

    const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3], P[11]);
    const float variation = fogDensityScale(p * float3(1, 2, 1) * asfloat(P[8].w) + asfloat(P[9].xyz), asfloat(P[8].z));
    float sigma = fogExtinctionAt(fog, p.y) * variation;
    if (zs * toRay < fog.start) sigma = 0;
    // the local volumes
    float3 albedo = fog.albedo;
    if (P[10].x != 0)
    {
        ByteAddressBuffer volumes = ResourceDescriptorHeap[P[9].w];
        float3 scattering = fog.albedo * sigma;
        [loop] for (uint i = 0; i < P[10].x; ++i)
        {
            const float4 r0 = asfloat(volumes.Load4(i * 96)), r1 = asfloat(volumes.Load4(i * 96 + 16)), r2 = asfloat(volumes.Load4(i * 96 + 32));
            const float3 u = float3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
            const uint4 c = volumes.Load4(i * 96 + 48);
            const float reach = (c.w >> 24) != 0 ? max(abs(u.x), max(abs(u.y), abs(u.z))) : length(u);
            if (reach >= 1.0) continue;
            const float4 turbulence = asfloat(volumes.Load4(i * 96 + 64));
            const uint4 source = volumes.Load4(i * 96 + 80);
            // the height above the source plane over the height left above it (plane 0: the height inside the volume)
            const float plane = asfloat(source.y), above = (0.5 * u.y + 0.5 - plane) / max(1.0 - plane, 1e-3);
            if (above < 0) continue;
            float s = asfloat(c.x) * saturate((1.0 - reach) * asfloat(c.z)) * exp2(-asfloat(c.y) * above) * variation;
            if (turbulence.w > 0)
                s *= fogSteamScale(u * turbulence.xyz + float3(float(i) * 19.0, -asfloat(source.x), float(i) * 7.0), turbulence.w * saturate(0.25 + 2.25 * above));
            if (source.w != 0 && P[10].w != 0xFFFFFFFFu) s *= fogVolumeGrid(P[10].w, source.z, source.w, u);
            sigma += s;
            scattering += s * float3(c.w & 0xFFu, (c.w >> 8) & 0xFFu, (c.w >> 16) & 0xFFu) * (1.0 / 255.0);
        }
        if (sigma > 0) albedo = scattering / sigma;
    }
    if (!clipPlaneKeeps(p)) sigma = 0;  // (a planar reflection view: the ray before the mirror is not a path of light)
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
            float lit = (1 - saturate(shadowed)) * cloudSunTransmittanceFromLut(P[6].w, p);
            if (P[10].z != 0xFFFFFFFFu && lit > 0)
            {
                ByteAddressBuffer hair = ResourceDescriptorHeap[P[10].z];
                lit *= hairTransmittance(P[10].z, p - hairDensityOrigin(hair), sun, 3.0e38f, 32u, jitter.z);
            }
            if (lit > 0) lit *= fxParticleShadow(P[11].w, p);
            inScattered += E * airSunTransmittance(a, P[6].w, airLiftToSurface(a, p), sun) * (lit * airMiePhase(dot(dir, sun), fog.g));
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
    float4 value = float4(albedo * inScattered * (sigma * g_exposure), sigma);  // (rgb: x the view's exposure)
    if (any(isnan(value)) || any(isinf(value))) value = 0;
    value.rgb = min(value.rgb, 65504.0);

    // history: the cell's centre in the previous view
    if (P[1].z != 0xFFFFFFFFu)
    {
        const float zc = min(fogDepthOfSlice(g, float(id.z) + 0.5), max(farthest - 0.02, 0.0));
        const float3 centre = g_cameraPosition + centreRay * zc;
        const float4 clip = mul(g_prevViewProj, float4(centre, 1));
        if (clip.w > 1e-4)
        {
            // the centre's place in the previous volume, whose cells reach past the view's edge where the view is not a
            // whole number of cells (1080 rows: the last row's centres lie on the edge - they keep their history too)
            const float2 scale = float2(g_viewWidth, g_viewHeight) / float2(g.x * g.cellPx, g.y * g.cellPx);
            const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * scale;
            const float slice = fogSliceOfDepth(g, clip.w);
            if (all(uv > 0) && all(uv < 1) && slice < float(g.z))
            {
                Texture3D<float4> history = ResourceDescriptorHeap[P[1].z];
                const float4 h = history.SampleLevel(g_linearClamp, float3(uv, clamp(slice / float(g.z), 0.5 / float(g.z), 1.0 - 0.5 / float(g.z))), 0);
                // (a < 0: cells that were hidden)
                if (!any(isnan(h)) && h.a >= 0) value = lerp(value, float4(min(h.rgb * asfloat(P[10].y), 65504.0), h.a), asfloat(P[7].w));
            }
        }
    }
    output[id] = value;
}
