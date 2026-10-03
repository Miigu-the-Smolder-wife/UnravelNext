// A9 layers at ray hits (HitShading.hlsli, HitLocalLights.hlsli): the hit's coat with its lobe widened by the hit's cone.
#ifndef UNX_RT_HIT_LAYERS_HLSLI
#define UNX_RT_HIT_LAYERS_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"

// Per thread: the half-width (tan) of the cone the hit's value is a mean over (GI texel rays set it; 0 = the point lobe).
static float g_rtHitCone = 0;
// The ray viewer's iris plane and caustic, shared by sun and local-light shading.
static uint g_rtEyeWord = 0;
float rtHitDiffuseCosine(GpuMaterial m, float3 n, float3 wi)
{
    const float cosine = dot(n, wi);
    if ((m.classFlags & MATERIAL_EYE) != 0 && modelEyeMask(g_rtEyeWord) > 0)
        return modelEyeCosine(modelEyeOf(g_rtEyeWord, n), cosine, wi);
    return max(cosine, 0.0);
}
ModelCoat rtHitCoat(GpuMaterial m)
{
    ModelCoat c = modelCoatOf(m);
    if (c.cover > 0 && g_rtHitCone > 0)
    {
        const float a = modelAlpha(c.roughness);
        c.roughness = sqrt(sqrt(a * a + g_rtHitCone * g_rtHitCone));
    }
    return c;
}

// The hit's material takes light from behind its shading normal: Foliage's transmission, a Subsurface material's light
// through thin parts (transmission > 0). Callers give such a hit its sun term and its light sample on either side, and
// choose its light without the hit's orientation.
bool rtHitTransmits(GpuMaterial m)
{
    const uint cls = m.classFlags & 0xFFu;
    return cls == MATERIAL_FOLIAGE || (cls == MATERIAL_SUBSURFACE && (m.transmission > 0 || (m.classFlags & MATERIAL_EYE) != 0));
}
#endif
