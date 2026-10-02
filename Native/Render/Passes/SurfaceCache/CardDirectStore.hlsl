// unx-kernel: cs_6_6 main
// unx-variants: SKY=0,1
// r.card.direct.store (CardLighting.hlsli): one group (8 x 8 threads) per tile of the frame's direct list, one thread per
// texel. The texel's direct irradiance = the sum over the tile's lights of the light's unshadowed irradiance at the texel
// (its integral over the light: mlLightUnshadowed with a Lambert point) x a punctual light's function toward the texel
// (SurfaceCacheLightFunction.hlsli: profile, cookie or gobo at the texel's angle from the light, keys and flicker at the
// frame's time) x its visible bit, plus the sun's; stored in the
// direct atlas, and the final lighting atlas gets (direct + indirect) x albedo / pi + emission. The group's first thread
// writes the tile's uniform bits: per light, whether the rays traced this update all agreed.
// P[0] = { card frame SRV, select SRV, frame index, flags (bit 10: lights without their shadow rays) }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli)
// P[4] = { tile lights SRV (raw), tile shadow SRV (raw), uniform bits UAV (raw), page capacity }
// P[5] = { direct list capacity, direct atlas UAV, final atlas UAV, indirect atlas SRV }
// P[6], P[7] = RtSceneSrvs (the light data: its header names the frame's light function table)
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Shading/MegaLightsSampling.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/SurfaceCache/SurfaceCacheLightFunction.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    const uint index = group.x;
    if (index >= min(select.Load(clSelectContext(0) + CL_SELECT_TILES), P[5].x)) return;
    const McFrame f = mcFrame(P[0].x);
    uint pageIndex;
    uint2 tile;
    clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 0, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    const uint2 coord = tile * MC_TILE + thread.xy;
    if (any(float2(coord) >= page.sizeInTexels)) return;
    const McCard card = mcLoadCard(f, page.card);
    const uint t = thread.x + thread.y * 8u;
    ByteAddressBuffer tileLights = ResourceDescriptorHeap[P[4].x];
    ByteAddressBuffer tileShadow = ResourceDescriptorHeap[P[4].y];
    const uint base = index * CL_TILE_LIGHT_BYTES, shadowBase = index * CL_TILE_SHADOW_BYTES;
    const uint4 state = tileLights.Load4(base + 32);  // valid mask (2), uniform slots, sun
    const bool unshadowed = (P[0].w & 1024u) != 0;

    if (t == 0)
    {
        // the uniform bits of this update: a slot whose traced texels were all visible or all blocked
        const uint2 even = uint2(0x00550055u, 0x00550055u);
        uint words[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        for (uint k = 0; k < CL_SLOTS; ++k)
        {
            uint bit = CL_SUN_BIT;
            if (k < CL_LIGHTS)
            {
                const uint light = tileLights.Load(base + k * 4);
                if (light == MC_NONE) continue;
                bit = light & 255u;
            }
            else if (state.w == 0) continue;
            const uint2 traced = ((state.z >> k) & 1u) != 0 ? state.xy & even : state.xy;
            const uint2 visible = tileShadow.Load2(shadowBase + k * 8) & traced;
            if (all(visible == 0) || all(visible == traced)) words[bit >> 5] |= 1u << (bit & 31u);
        }
        RWByteAddressBuffer uniformBits = ResourceDescriptorHeap[P[4].z];
        const uint2 atlasTile = (uint2(page.atlasRect.xy) + tile * MC_TILE) / MC_TILE;
        const uint at = (atlasTile.x + atlasTile.y * (f.atlasSize / MC_TILE)) * CL_UNIFORM_BYTES;
        uniformBits.Store4(at, uint4(words[0], words[1], words[2], words[3]));
        uniformBits.Store4(at + 16, uint4(words[4], words[5], words[6], words[7]));
    }

    const McTexel texel = mcPageTexel(f, page, card, coord);
    RWTexture2D<float3> direct = ResourceDescriptorHeap[P[5].y];
    RWTexture2D<float3> final = ResourceDescriptorHeap[P[5].z];
    if (!texel.valid)
    {
        direct[texel.atlas] = 0;
        final[texel.atlas] = 0;
        return;
    }
    float3 e = 0;
    const MlPoint lambert = mlPointLambert(texel.position, texel.normal, float3(1, 1, 1));  // (its radiance is irradiance / pi)
    const uint functions = scLightFunctionTable(rtSceneSrvs(P[6], P[7]).pad);
    const float texelSize = 2 * max(page.cardUvTexelScale.x * card.extent.x, page.cardUvTexelScale.y * card.extent.y);
    [loop] for (uint k = 0; k < CL_LIGHTS; ++k)
    {
        const uint light = tileLights.Load(base + k * 4);
        if (light == MC_NONE) break;
        const GpuLight g = loadLight(light);
        if (lightCastsShadow(g) && !unshadowed)
        {
            const uint from = ((state.z >> k) & 1u) != 0 ? t & ~9u : t;
            if (((tileShadow.Load(shadowBase + k * 8 + (from >> 5) * 4) >> (from & 31u)) & 1u) == 0) continue;
        }
        float3 el = 3.14159265 * mlLightUnshadowed(lambert, g, light, UNX_NONE);
        if (any(el > 0)) el *= scLightFunction(functions, g, light, texel.position, texelSize);
        if (!all(el >= 0) || !all(el < 1e30)) el = 0;  // (NaN, infinite: no light)
        e += el;
    }
    if (state.w != 0)
    {
        const float muS = dot(texel.normal, normalize(g_sunDirection));
        const uint from = ((state.z >> CL_SUN_SLOT) & 1u) != 0 ? t & ~9u : t;
        if (muS > 0 && ((tileShadow.Load(shadowBase + CL_SUN_SLOT * 8 + (from >> 5) * 4) >> (from & 31u)) & 1u) != 0)
        {
            float3 es = giSunIlluminance(texel.position) * muS;
            if (!all(es >= 0) || !all(es < 1e30)) es = 0;
            e += es;
        }
    }
    direct[texel.atlas] = e * CL_IRRADIANCE_SCALE;
    Texture2D<float3> indirect = ResourceDescriptorHeap[P[5].w];
    Texture2D<float4> albedo = ResourceDescriptorHeap[f.albedo];
    Texture2D<float3> emissive = ResourceDescriptorHeap[f.emissive];
    const int3 at = int3(texel.atlas, 0);
    const float3 irradiance = e + indirect.Load(at) / CL_IRRADIANCE_SCALE;
    final[texel.atlas] = (irradiance * mcDecodeAlbedo(albedo.Load(at).rgb) / 3.14159265 + mcDecodeEmissive(emissive.Load(at))) * CL_RADIANCE_SCALE;
}
