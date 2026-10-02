// unx-kernel: ps_6_6 main
// r.card.capture (CardCapture.hlsli): the card texel's material. Targets:
//   0 albedo   RGBA8  sqrt(diffuse reflectance + 0.45 x specular colour) - what a rough bounce off the surface carries
//                     (Unreal's card albedo: DiffuseColor + SpecularColor x 0.45), a = 1. A layered material (A9) is
//                     captured through its layers, seen along the normal as the reference's layered capture is
//                     (LumenCardBasePass.ush: V = L = N, the layers' weights by their view transmittance): under a
//                     clearcoat the base's light passes the coat twice and the coat's own reflection is added
//                     (ccLayered: the coat's roughness sets both); a sheen adds its tint x its albedo and keeps the rest
//                     of the base; a thin film replaces the specular colour by the film's at normal incidence. The
//                     readers of the cards' final lighting (the radiosity, the translucency volume) then bounce what a
//                     hit that shades its own material bounces (HitShading.hlsli)
//   1 normal   RG8    the shading normal's x, y in card space x 0.5 + 0.5 (z >= 0: toward the capture side)
//   2 emissive R11G11B10F  emission in nits x MC_EMISSIVE_SCALE (0 for a visible-only emissive: its analytic light
//                     lights the scene, INTERFACES v1.92)
// Textures by the pixel's own derivatives (the card's texel footprint). Material classes: Standard, Foliage,
// Subsurface (their base layer); Terrain (the splat-weighted layers' base colour and metallic); Cut (triplanar base
// colour). Water, Glass and Hair are not in the cache (their draws are skipped on the CPU).
#include "Passes/SurfaceCache/CardCaptureMaterial.hlsli"

struct CardPixel
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 emissive : SV_Target2;
};

CardPixel main(CardVertex i)
{
    CcSurface s;
    s.material = P[0].w;
    s.tableSrv = P[4].x;
    s.uv = i.uv;
    s.uvDx = ddx(i.uv);
    s.uvDy = ddy(i.uv);
    s.normal = i.normal;
    s.tangent = i.tangent;
    s.scaled = i.scaled;
    s.scaledDx = ddx(i.scaled);
    s.scaledDy = ddy(i.scaled);
    s.direction = P[2].w & 7u;
    s.twoSided = ((P[2].w >> 8) & 1u) != 0;
    const CcMaterial m = ccMaterial(s);
    if (!m.covered) discard;
    CardPixel o;
    o.albedo = m.albedo;
    o.normal = m.normal;
    o.emissive = m.emissive;
    return o;
}
