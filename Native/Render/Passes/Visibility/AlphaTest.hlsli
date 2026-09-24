// Alpha test shared by V's visibility pixel kernel and the depth raster service (INTERFACES_KO.md 8.1): an
// alpha-tested material covers a point where its baseColor texture alpha >= alphaCutoff. Owner: V.
#ifndef UNX_ALPHA_TEST_HLSLI
#define UNX_ALPHA_TEST_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"

bool alphaTestCovered(uint material, float2 uv)
{
    const GpuMaterial m = loadMaterial(material);
    if ((m.classFlags & MATERIAL_ALPHA_TESTED) == 0 || m.baseColorTexture == UNX_NONE) return true;
    Texture2D<float4> baseColor = ResourceDescriptorHeap[m.baseColorTexture];
    return baseColor.Sample(g_anisoWrap, uv).a >= m.alphaCutoff;
}

#endif
