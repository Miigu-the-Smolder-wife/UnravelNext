// unx-kernel: ps_6_6 main
// s.vsm.tint (shadow.vsm.translucent_tint; VsmTint.hlsli): the pixel kernel of V's raster service for the sun levels'
// glass casters. One tint texel per fragment: rgb = what the surface lets through along the sun's direction, a = the
// view's depth (v: nearer the sun = larger); the request's blend multiplies the rgb into the texel and keeps the largest a.
//   a pane (a two-sided glass material: one surface stands for both faces): T = (1 - F)^2 t / (1 - F^2 t^2), the pane's
//           transmittance with its inner reflections, t = the base colour (x its texture) - the pane M's translucent
//           layer shades (TranslucentComposite.hlsl);
//   a solid (one-sided: the light crosses a front and a back face, both drawn): (1 - F) sqrt(t) per face - the two
//           faces' Fresnel losses and the tint once; the body's absorption over its thickness is not known here.
// F: the dielectric's reflectance at the sun's angle on the interpolated normal (DepthRasterRequest::pixelNormals).
// P[4] = asfloat: the direction towards the sun (xyz), 0
#define DEPTH_RASTER_NORMALS 1
#include "Passes/Visibility/DepthRaster.hlsli"

float tintFresnel(float cosI, float eta)
{
    cosI = saturate(cosI);
    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    const float cosT = sqrt(1.0 - sin2T);
    const float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (eta * cosT - cosI) / (eta * cosT + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

float4 main(DepthRasterPixel p) : SV_Target0
{
    const float2 uvDx = ddx(p.uv), uvDy = ddy(p.uv);
    if (!depthRasterCovered(p)) discard;
    const GpuMaterial m = loadMaterial(p.material);
    const float3 t = saturate(m.baseColor * materialBaseColorGrad(m, p.uv, uvDx, uvDy).rgb);
    const float3 n = dot(p.normal, p.normal) > 1e-20 ? normalize(p.normal) : asfloat(P[4].xyz);
    const float F = tintFresnel(abs(dot(n, asfloat(P[4].xyz))), 1.0 / max(m.ior, 1.0001));
    float3 through;
    if ((m.classFlags & MATERIAL_TWO_SIDED) != 0) through = (1 - F) * (1 - F) * t / max(1 - F * F * t * t, 1e-6);
    else through = (1 - F) * sqrt(t);
    return float4(saturate(through), p.position.z);
}
