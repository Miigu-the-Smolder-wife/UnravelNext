// Alpha test shared by V's visibility pixel kernel and the depth raster service (INTERFACES_KO.md 8.1): an
// alpha-tested material covers a point where its baseColor texture alpha >= alphaCutoff. The texture is read as M's
// material resolve reads it (MaterialTextures.hlsli: its addressing, footprint filtering over the pixel's uv gradients,
// coverage-preserving alpha mips), so the cut shape is the same at every distance and in every view. Pixel shaders only
// (uv derivatives). Owner: V.
#ifndef UNX_ALPHA_TEST_HLSLI
#define UNX_ALPHA_TEST_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"

bool alphaTestCovered(uint material, float2 uv)
{
    const GpuMaterial m = loadMaterial(material);
    if ((m.classFlags & MATERIAL_ALPHA_TESTED) == 0) return true;
    return materialBaseColorGrad(m, uv, ddx(uv), ddy(uv)).a >= m.alphaCutoff;  // no texture: 1, covered
}

#endif
