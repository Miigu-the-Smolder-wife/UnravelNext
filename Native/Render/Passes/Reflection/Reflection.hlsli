// Reflections of the main view (R track, ARCHITECTURE 2.6; INTERFACES 5.6 v1.2 public API: reflectionRadiance,
// reflectionLobeHalfAngle). Consumer: M's shading kernel.
//
// Three paths by lobe bandwidth (design 2.6):
//   K  narrow lobe half-angle >= 22 deg: M evaluates screenProbeRadiance(..., reflectionLobeHalfAngle(r, NoV)) itself
//      (reflectionRadiance(...).a == 0 for these pixels);
//   G  narrower glossy lobes: R traces 4 GGX rays per sample on a grid whose spacing follows the lobe's screen blur,
//      with the cache as control variate, and resolves per pixel;
//   M  mirrors (blur < 1 px or roughness < reflection.mirror_roughness_max): one ray per pixel.
//   Planar mirrors and calm water: a reflection camera raster through FrameServices::renderView (5.4), composited here.
//
// view.reflection: RGBA16F, width W, height H + ceil(H / 8). Rows [0, H): per-pixel lobe-normalised incident radiance
// (nits; M multiplies the material model's specular directional albedo) and a = 1 (valid) or 0 (K pixel). Rows
// [H, H + ceil(H/8)): one texel per 8 x 8 tile, a = 1 when R wrote that tile's pixels (untouched tiles hold no data and
// are K everywhere, so the texture is never cleared).
#ifndef UNX_REFLECTION_HLSLI
#define UNX_REFLECTION_HLSLI
#include "Bindless.hlsli"

// Half-angle (radians) of the lobe's narrow axis, 75 % energy: theta_r = 2 atan(sqrt(3) alpha), alpha = max(r^2, 1e-4)
// (INTERFACES 8.1), compressed by cos(theta_o) = NoV perpendicular to the plane of incidence at grazing angles.
float reflectionLobeHalfAngle(float perceptualRoughness, float NoV)
{
    const float alpha = max(perceptualRoughness * perceptualRoughness, 1e-4);
    return 2.0 * atan(1.7320508 * alpha) * saturate(NoV);
}

// Height of the pixel rows of view.reflection (the texture adds ceil(H / 8) tile rows): floor(T * 8 / 9) = H exactly.
uint reflectionPixelRows(uint textureHeight) { return textureHeight * 8u / 9u; }

// rgb = lobe-normalised incident radiance (G/M paths, planar mirrors), a = 1 when valid; a = 0: K path (M evaluates it).
float4 reflectionRadiance(uint reflectionSrv, uint2 pixel)
{
    Texture2D<float4> t = ResourceDescriptorHeap[reflectionSrv];
    uint width, height;
    t.GetDimensions(width, height);
    const uint rows = reflectionPixelRows(height);
    if (t.Load(int3(pixel.x / 8, rows + pixel.y / 8, 0)).a < 0.5) return 0;
    return t.Load(int3(pixel, 0));  // a written per pixel in R's tiles: 1 = G/M/planar result, 0 = K pixel
}

#endif
