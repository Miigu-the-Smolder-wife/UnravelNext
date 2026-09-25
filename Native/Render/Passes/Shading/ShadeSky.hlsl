// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Sky pixels (no surface in the vis buffer), one 8 x 8 tile of the sky class list per group: S's sky radiance of the
// pixel direction plus the solar disk (uniform radiance, INTERFACES 8.3) weighted by its coverage of the pixel.
// Without S's atmosphere (tracks built alone) the sky is black and the disk has the top-of-atmosphere radiance.
// P[0] = { material word, color UAV, tile lists (raw), list offset (entries) }
// P[1] = { atmosphere transmittance, multi-scatter, sky view, aerial } (UNX_NONE = absent)
// P[2] = { froxel lights (raw), froxel volume } (UNX_NONE = absent): shafts in front of the sky (depth clamps to far_m)
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].z];
    const uint tile = tiles.Load(4 * (P[0].w + gid.x));
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].x];
    if (mWordMaterial(words[pixel]) != M_MATERIAL_SKY) return;

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
        radiance = atmosphereSkyRadiance(atm, dir);
        sun = atmosphereSunRadiance(atm, g_cameraPosition);
    }
    else
    {
        const float sinS = sin(g_sunAngularRadius);
        radiance = 0;
        sun = g_sunIlluminance * g_sunColor / (SH_PI * sinS * sinS);
    }
    radiance += sun * shSunDiskCoverage(D, Dx, Dy);
    if (P[2].y != UNX_NONE && g_viewKind == VIEW_MAIN)
    {
        FroxelSrvs froxels;
        froxels.lights = P[2].x;
        froxels.lightIndices = P[2].x;
        froxels.scattering = P[2].y;
        froxels.pad = 0;
        const float4 air = froxelScattering(froxels, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), 1e30);
        radiance = radiance * air.a + air.rgb;
    }

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].y];
    color[pixel] = shEncodeOutput(radiance);
}
