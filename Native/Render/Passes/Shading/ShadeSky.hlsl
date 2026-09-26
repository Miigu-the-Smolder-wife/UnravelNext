// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Sky pixels (no surface in the vis buffer), one 8 x 8 tile of the sky class list per group: S's sky radiance of the
// pixel direction with the view's air (atmosphereSkyRadianceView: the far-field sky plus the air volume's sky
// correction, i.e. local lights' glow against the sky and the scattering casters' shadows remove, as shafts) plus the
// solar disk (uniform radiance, INTERFACES 8.3) weighted by its coverage of the pixel.
// Without S's atmosphere (tracks built alone) the sky is black and the disk has the top-of-atmosphere radiance.
// P[0] = { material word, color UAV, tile lists (raw), list offset (entries) }
// P[1] = { atmosphere transmittance, multi-scatter, sky view, this view's air volume } (UNX_NONE = absent)
// P[2] = { experiment mask (shading.experiment_disable; not read here since edge detection moved to EdgeDetect), 0, 0, 0 }
// P[3] = { 0, 0, edge tile mask SRV (EdgeDetect.hlsl; UNX_NONE = no edge pixels), 0 }, P[7].x edge radiance UAV
// (RGBA16F): a sky edge pixel (a neighbour shows a surface) keeps its exposed linear radiance for the composite, and so
// does every pixel of a tile with coverage fragments (P[6].y: V's coverage tiles, UNX_NONE = no coverage layer), whose
// band A remainder the coverage composite adds (CoverageComposite.hlsl).
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
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
        radiance = atmosphereSkyRadianceView(atm, dir, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight));
        sun = atmosphereSunRadiance(atm, g_cameraPosition);
    }
    else
    {
        const float sinS = sin(g_sunAngularRadius);
        radiance = 0;
        sun = g_sunIlluminance * g_sunColor / (SH_PI * sinS * sinS);
    }
    radiance += sun * shSunDiskCoverage(D, Dx, Dy);
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].y];
    color[pixel] = shEncodeExposed(shParticles(radiance * g_exposure, pixel, P[5].z, P[5].w));  // P[5].zw particle layer
    return radiance;
}
