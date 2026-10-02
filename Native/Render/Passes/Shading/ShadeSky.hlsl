// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Sky pixels (no surface in the vis buffer), one 8 x 8 tile of the sky class list per group: S's sky radiance of the
// pixel direction with the view's air (atmosphereSkyRadianceView: the far-field sky plus the air volume's sky
// correction, i.e. local lights' glow against the sky and the scattering casters' shadows remove, as shafts) plus the
// solar disk (uniform radiance, INTERFACES 8.3) weighted by its coverage of the pixel (not in planar reflection views).
// Without S's atmosphere (tracks built alone) the sky is black and the disk has the top-of-atmosphere radiance.
// P[0] = { material word, color UAV, tile lists (raw), list offset (entries) }
// P[1] = { atmosphere transmittance, multi-scatter, sky view, this view's air volume } (UNX_NONE = absent)
// P[2] = { experiment mask (shading.experiment_disable; not read here since edge detection moved to EdgeDetect), 0, 0, 0 }
// P[3] = { 0, 0, edge tile mask SRV (EdgeDetect.hlsl; UNX_NONE = no edge pixels), 0 }, P[7].x edge radiance UAV, P[7].w
// V's water layer vis ids (v1.75; UNX_NONE = none: a sky pixel under a water-layer stream surface keeps its radiance, W's
// refraction source)
// (RGBA16F): a sky edge pixel (a neighbour shows a surface) keeps its exposed linear radiance for the composite, and so
// does every pixel of a tile with coverage fragments (P[6].y: V's coverage tiles, UNX_NONE = no coverage layer), whose
// band A remainder the coverage composite adds (CoverageComposite.hlsl).
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Atmosphere/Celestial.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

float3 shadeSky(uint2 pixel, Texture2D<uint> words);

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].z];
    const uint tile = tiles.Load(4 * (P[0].w + gid.x));
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].x];
    const bool active = all(pixel < uint2(g_viewWidth, g_viewHeight)) && mWordMaterial(words[min(pixel, uint2(g_viewWidth, g_viewHeight) - 1)]) == M_MATERIAL_SKY;
    if (!active) return;
    const float3 radiance = shadeSky(pixel, words);
    const uint2 tileCoord = uint2(tile & 0xFFFFu, tile >> 16);
    bool keep = false;
    if (P[6].y != UNX_NONE)
    {
        ByteAddressBuffer coverage = ResourceDescriptorHeap[P[6].y];
        keep = coverage.Load(4 * ((tileCoord.x + tileCoord.y * ((g_viewWidth + 7) / 8)) * COV_TILE_WORDS + COV_TILE_COUNT)) != 0;
    }
    if (!keep && P[3].z != UNX_NONE)
    {
        Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[3].z];
        const uint2 edgeMask = edgeTiles[tileCoord];
        const uint bit = tid.y * M_TILE + tid.x;
        keep = (((bit < 32 ? edgeMask.x : edgeMask.y) >> (bit & 31)) & 1u) != 0;
    }
    if (!keep && P[7].w != UNX_NONE)
    {
        Texture2D<uint> waterVis = ResourceDescriptorHeap[P[7].w];
        keep = (waterVis[pixel] >> 30) == 3u;  // COV_STREAM_ID: W's water surfaces
    }
    if (keep)
    {
        RWTexture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[7].x];
        edgeRadiance[pixel] = float4(radiance * g_exposure, 1);
    }
}

// The pixel's sky radiance (linear, before exposure); writes the output.
float3 shadeSky(uint2 pixel, Texture2D<uint> words)
{

    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 dir = normalize(D);
    AtmosphereSrvs atm;
    atm.transmittance = P[1].x;
    atm.multiScatter = P[1].y;
    atm.skyView = P[1].z;
    atm.aerial = P[1].w;
    float3 radiance;
    float3 sun;
    if (atm.transmittance != UNX_NONE)
    {
        radiance = atmosphereSkyRadianceClouded(atm, dir, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight));  // B5: the view sky with the cloud layer (none: atmosphereSkyRadianceView)
        sun = atmosphereSunRadiance(atm, g_cameraPosition);
    }
    else
    {
        const float sinS = sin(g_sunAngularRadius);
        radiance = 0;
        sun = g_sunIlluminance * g_sunColor / (SH_PI * sinS * sinS);
    }
    // S's celestial objects (v1.49, B4; P[2].y = FrameResources::celestial): the moon with its phase, the stars, the airglow.
    // When the frame's directional light is the moon, g_sun* describe the moon: its disk is drawn by atmosphereCelestial
    // with its phase, not as a uniform disk.
    radiance += atmosphereCelestial(atm, P[2].y, D, Dx, Dy);
    // Planar reflection views leave the disk out: their reader adds the sun's specular lobe analytically over the disk
    // (M's mirror pixels, W's calm water), as R's reflection rays exclude it - drawn here it would count twice.
    if (!celestialMoonHoldsLight(P[2].y) && g_viewKind != VIEW_PLANAR_REFLECTION) radiance += sun * shSunDiskCoverage(D, Dx, Dy);
    // The height fog over the sky (atmosphere.fog.sky_amount; 0: the sky is left alone): along the ray to the far slices' end.
    {
        FogParams fogParams;
        if (fogLoad(fogParams) && fogParams.skyAmount > 0)
        {
            const float4 fog = fogAt((float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), 3.0e38);
            radiance = lerp(radiance, radiance * fog.a + fog.rgb, fogParams.skyAmount);
        }
    }
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].y];
    shExposureHistogram(P[4].w, radiance, pixel, asfloat(P[4].z));  // P[4].w histogram, P[4].z centre sigma (main view)
    color[pixel] = shEncodeExposed(shParticles(radiance * g_exposure, pixel, P[5].z, P[5].w));  // P[5].zw particle layer
    return radiance;
}
