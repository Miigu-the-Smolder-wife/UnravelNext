// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Sky pixels (no surface in the vis buffer), one 8 x 8 tile of the sky class list per group: S's sky radiance of the
// pixel direction plus the solar disk (uniform radiance, INTERFACES 8.3) weighted by its coverage of the pixel.
// Without S's atmosphere (tracks built alone) the sky is black and the disk has the top-of-atmosphere radiance.
// P[0] = { material word, color UAV, tile lists (raw), list offset (entries) }
// P[1] = { atmosphere transmittance, multi-scatter, sky view, aerial } (UNX_NONE = absent)
// P[2] = { experiment mask (shading.experiment_disable), 0, 0, 0 }
// P[3] = { 0, 0, edge args UAV (raw), vis id SRV }, P[7] as ShadeOpaque
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"

bool shadeSky(uint2 pixel, Texture2D<uint> words);

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].z];
    const uint tile = tiles.Load(4 * (P[0].w + gid.x));
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].x];
    const bool active = all(pixel < uint2(g_viewWidth, g_viewHeight)) && mWordMaterial(words[min(pixel, uint2(g_viewWidth, g_viewHeight) - 1)]) == M_MATERIAL_SKY;
    bool isEdgeLane = false;
    if (active) isEdgeLane = shadeSky(pixel, words);
    edgeAppendPixel(pixel, isEdgeLane, P[7].w, P[3].z);
}

bool shadeSky(uint2 pixel, Texture2D<uint> words)
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
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].y];
    color[pixel] = shEncodeOutput(radiance);
    // Edge: the sky has one vis id (VIS_NONE), so a neighbour with another shows a surface.
    if (P[7].x == UNX_NONE || (P[2].x & 256)) return false;
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[3].w];
    bool isEdge = false;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const int2 q = int2(pixel) + int2(int(k % 3) - 1, int(k / 3) - 1);
        if (k != 4 && all(q >= 0) && q.x < int(g_viewWidth) && q.y < int(g_viewHeight)) isEdge = isEdge || visIds[uint2(q)] != VIS_NONE;
    }
    if (isEdge)
    {
        RWTexture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[7].x];
        edgeRadiance[pixel] = float4(radiance * g_exposure, 1);
    }
    return isEdge;
}
