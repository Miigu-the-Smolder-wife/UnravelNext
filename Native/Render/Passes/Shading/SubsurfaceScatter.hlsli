// Subsurface class, stage B: screen-space subsurface scattering of the class's diffuse light (shading.subsurface_scatter;
// the structure follows ue6-main PostProcessSubsurface.usf and SubsurfaceBurleyNormalized.ush, read as a reference; the
// code is ours). The class's kernels keep the diffuse light apart from the specular light and the emission, per unit f_d
// (the albedo comes back after the scattering, so texture detail stays):
//   m.sss.clear           the diffuse texture to 0 (a = 0: not a Subsurface pixel);
//   m.ml.spatial.sss      shading.mega_lights: the local lights' filtered diffuse into it, their specular apart
//                         (MegaLightsSpatialSubsurface.hlsl);
//   part 1 (SubsurfaceDirect.hlsl) and part 2 (SubsurfaceIndirect.hlsl) of ShadeOpaque.hlsl add theirs: the sun, the
//                         local lights without mega_lights, the tile term, the emissive irradiance, the light through
//                         thin parts, the indirect irradiance; part 2 sets a = the pixel's view depth and leaves the
//                         specular light and the emission in the direct radiance texture;
//   m.sss.scatter         (SubsurfaceScatter.hlsl, on the class's tiles) the function below per pixel, then
//                         colour = specular + emission + f_d x scattered, and what the plain path does from there: the
//                         air, the exposure histogram, particles, the output, the edge / coverage radiance.
// The diffuse texture: RGBA16F, rgb = E_d x exposure (the pixel's diffuse radiance is f_d E_d, f_d = (1 - metallic)
// baseColor / pi), a = view depth (m; 0: no Subsurface pixel).
// Per pixel (the model and its sampling: Passes/Common/SubsurfaceProfile.hlsli, scene::model):
//   plane      the surface's plane at the pixel from the depth buffer (each axis's neighbour nearer in depth), so normal
//              maps do not tilt it; samples lie in it - a disk on the surface, an ellipse on the screen, the same
//              footprint at every view angle (a screen-space disk needs the depth weight to take back what
//              foreshortening spreads, and spends its samples there);
//   footprint  the profile's mean radius 2.5 d of the widest channel over the pixel's size at its depth: under
//              'minPixels' the pixel keeps its own diffuse light and takes no sample; the scattering fades in up to
//              twice that (no line where it starts);
//   samples    'samples' / 2 pairs beyond the pixel's own footprint, radii by the profile's inverse distribution, one
//              rotation and one radial offset per pixel from the frame's blue noise (the temporal upscale accumulates
//              the frames; without it the pattern stays);
//   rejection  a sample on a pixel of another class (a = 0), outside the view, or farther than SSS_PLANE_REACH d (plus
//              the depth's f16 step and two pixels) from the plane - another surface - is left out of both sums: no
//              light crosses a silhouette or a depth discontinuity, and the remaining samples keep the energy.
#ifndef UNX_M_SUBSURFACE_SCATTER_HLSLI
#define UNX_M_SUBSURFACE_SCATTER_HLSLI
#include "Frame.hlsli"
#include "Passes/Common/BlueNoise.hlsli"
#include "Passes/Common/SubsurfaceProfile.hlsli"

#define SSS_PLANE_REACH 2.0  // in units of the widest channel's d: beyond it a sample point is on another surface

// Unit normal of the surface's plane at the pixel, towards the viewer: the cross product of the position differences
// to the horizontal and the vertical neighbour nearer in view depth (the other one may lie across a silhouette). 'n'
// (the shading normal) where the depth gives none: a degenerate cross product or a plane seen edge-on.
float3 sssSurfaceNormal(Texture2D<float> depthTex, uint2 pixel, float3 D, float3 Dx, float3 Dy, float linearZ, float3 n)
{
    const int2 p = int2(pixel), last = int2(g_viewWidth, g_viewHeight) - 1;
    const float far = 1e30;
    const float zl = p.x > 0 ? linearDepth(depthTex[p + int2(-1, 0)]) : far, zr = p.x < last.x ? linearDepth(depthTex[p + int2(1, 0)]) : far;
    const float zu = p.y > 0 ? linearDepth(depthTex[p + int2(0, -1)]) : far, zd = p.y < last.y ? linearDepth(depthTex[p + int2(0, 1)]) : far;
    const float3 P0 = D * linearZ;
    const float3 dx = abs(zl - linearZ) < abs(zr - linearZ) ? P0 - (D - Dx) * zl : (D + Dx) * zr - P0;
    const float3 dy = abs(zu - linearZ) < abs(zd - linearZ) ? P0 - (D - Dy) * zu : (D + Dy) * zd - P0;
    float3 g = cross(dx, dy);
    const float len = length(g);
    if (!(len > 0) || !(len < far)) return n;
    g /= len;
    if (dot(g, D) > 0) g = -g;
    return dot(g, D) < -0.05 * length(D) ? g : n;
}

// One sample at the plane point Q (camera-relative): the pixel it projects to, that pixel's diffuse light and its point's
// distance from the plane. False: no light from there (see the header).
bool sssTap(Texture2D<float4> diffuseTex, uint2 pixel, float3 D, float3 Dx, float3 Dy, float2 invStep, float3 P0, float3 plane, float reach, float3 Q,
            out float3 light, out float h)
{
    light = 0;
    h = 0;
    const float depthQ = -dot(Q, g_view[2].xyz);  // along the view axis (D has unit depth along it)
    if (!(depthQ > g_nearPlane)) return false;
    const float3 u = Q / depthQ - D;
    const int2 s = int2(floor(float2(pixel) + 0.5 + float2(dot(u, Dx), dot(u, Dy)) * invStep));
    if (any(s < 0) || any(s >= int2(g_viewWidth, g_viewHeight))) return false;
    const float4 e = diffuseTex[s];
    if (!(e.a > 0)) return false;
    const int2 o = s - int2(pixel);
    h = dot((D + Dx * o.x + Dy * o.y) * e.a - P0, plane);
    light = e.rgb;
    return abs(h) <= reach;
}

// The pixel's diffuse light E_d after scattering, in the diffuse texture's units. D, Dx, Dy: the pixel's ray (mPixelRay),
// linearZ its view depth, n its shading normal, albedo = (1 - metallic) baseColor, meanFreePath the material's (m, rgb).
float3 sssScatter(Texture2D<float4> diffuseTex, Texture2D<float> depthTex, uint2 pixel, float3 D, float3 Dx, float3 Dy, float linearZ, float3 n, float3 albedo,
                  float3 meanFreePath, uint samples, float minPixels)
{
    const float3 own = diffuseTex[pixel].rgb;
    const float3 d = sssDistance(meanFreePath, albedo);
    const float dS = max(d.r, max(d.g, d.b));
    const float footprint = linearZ * length(Dx);  // a pixel's size on a surface facing the viewer at this depth (m)
    const float strength = saturate(SSS_MEAN_RADIUS * dS / (footprint * minPixels) - 1);
    const uint pairs = samples / 2;
    if (!(strength > 0) || pairs == 0) return own;

    const float3 plane = sssSurfaceNormal(depthTex, pixel, D, Dx, Dy, linearZ, n);
    const float3 t = normalize(abs(plane.z) < 0.9 ? cross(float3(0, 0, 1), plane) : cross(float3(1, 0, 0), plane)), b = cross(plane, t);
    const float3 P0 = D * linearZ;
    // the pixel's own footprint in the plane as a disk of its area (the pixel's square, stretched by 1 / cos of the view angle)
    const float rc = footprint * 0.5642 * rsqrt(max(-dot(plane, D) / length(D), 0.25));
    const float3 centre = sssRadialCdf3(d, rc);
    const float centreS = sssRadialCdf(dS, rc);
    const float reach = SSS_PLANE_REACH * dS + 2 * footprint + linearZ * (1.0 / 512.0);
    const float2 invStep = 1 / float2(dot(Dx, Dx), dot(Dy, Dy));
    // (a shifted pixel and the tile's other two channels: the gathers that made this light read the tile at the pixel)
    const float2 noise = blueNoise4(pixel + uint2(17u, 43u), g_upscaleRatio > 0 ? g_frameIndex : 0u).zw;
    float3 sum = 0, weight = 0;
    [loop] for (uint k = 0; k < pairs; ++k)
    {
        const float r = sssSampleRadius(dS, centreS, k, pairs, noise.x);
        const float pdf = sssRadialPdf(dS, r);
        float sn, cs;
        sincos(sssSampleAngle(k, noise.y), sn, cs);
        const float3 q = (t * cs + b * sn) * r;
        [unroll] for (uint side = 0; side < 2; ++side)
        {
            float3 light;
            float h;
            if (sssTap(diffuseTex, pixel, D, Dx, Dy, invStep, P0, plane, reach, P0 + (side == 0 ? q : -q), light, h))
            {
                const float3 w = sssSampleWeight(d, r, h, pdf);
                sum += light * w;
                weight += w;
            }
        }
    }
    const float3 tail = float3(weight.r > 0 ? sum.r / weight.r : own.r, weight.g > 0 ? sum.g / weight.g : own.g, weight.b > 0 ? sum.b / weight.b : own.b);
    return lerp(own, lerp(tail, own, centre), strength);
}

#endif
