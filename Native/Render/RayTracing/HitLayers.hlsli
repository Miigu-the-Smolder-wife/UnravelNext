// A9 layers at ray hits (HitShading.hlsli, HitLocalLights.hlsli): the hit's coat with its lobe widened by the hit's cone.
#ifndef UNX_RT_HIT_LAYERS_HLSLI
#define UNX_RT_HIT_LAYERS_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"

// Per thread: the half-width (tan) of the cone the hit's value is a mean over (GI texel rays set it; 0 = the point lobe).
static float g_rtHitCone = 0;
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
#endif
