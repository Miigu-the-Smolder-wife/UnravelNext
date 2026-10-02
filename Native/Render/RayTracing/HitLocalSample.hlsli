// The local lights at a ray hit that the surface cache did not light (a hit without cards: a deforming instance, or a
// surface no card of its mesh sees - the inner sides of a recess, the far face of a concave part): one light chosen at
// the point by importance (HitLocalLights.hlsli) and its shadow ray. The reference lights such a hit with nothing at
// all; measured in the lobby (2026-10-02, gi.experiment_disable 2097152) 20 to 35 % of the gather's surface hits read
// no card, among them the coffers' inner sides that the strip lights light most - their light was missing from every
// bounce and from mirrors. The sample is unbiased for the sum of the cell's lights; its one-light noise is averaged by
// the reader (the probes' rays and filters, the reflections' reuse and history).
// A thread that calls this traces one more ray: the dispatches count 3 rays a thread (the ray, the sun's, this one).
// The includer is a ray library (RayShaders.hlsli: rtShadowTransmittance) with HitShading.hlsli and HitLocalLights.hlsli.
#ifndef UNX_RT_HIT_LOCAL_SAMPLE_HLSLI
#define UNX_RT_HIT_LOCAL_SAMPLE_HLSLI

float rtHitUnit(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}

// The hit's outgoing radiance toward v from the sampled light (RtHitLighting::local). footprint: the ray cone's width at
// the hit (the light function's footprint); bias: the shadow ray's offset along the geometric normal.
float3 rtHitLocalSample(RtSceneSrvs scene, RtSurface s, GpuMaterial m, float3 v, float footprint, float bias, uint seed)
{
    const float u0 = rtHitUnit(seed), u1 = rtHitUnit(seed), u2 = rtHitUnit(seed);
    const bool oriented = !rtHitTransmits(m);
    const RtLocalSample ls = rtLocalLightFinish(scene, rtLocalLightChooseOriented(scene, s.position, s.normal, !oriented, u0), s.position, u1, u2, footprint);
    if (!ls.valid) return 0;
    // the specular lobe toward the light widened by the ray's cone (the texel holds the cone's mean)
    GpuMaterial mc = m;
    const float alpha = modelAlpha(m.roughness);
    mc.roughness = sqrt(sqrt(alpha * alpha + g_rtHitCone * g_rtHitCone));
    const float3 f = rtLocalLightBrdfCos(mc, s.normal, v, ls.wi, false);
    if (!any(f > 0)) return 0;
    // (the light through the Glass on the way: RayShaders.hlsli rtShadowTransmittance - 0 or 1 in a library that did not
    // ask for the panes' transmittance)
    if (!ls.castShadow) return f * ls.weight;
    return f * ls.weight * rtShadowTransmittance(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, bias), RT_MASK_HIT_SHADOW);
}
#endif
