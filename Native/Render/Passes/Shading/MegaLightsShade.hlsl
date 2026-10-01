// unx-kernel: cs_6_6 main
// unx-variants: AREA=0,1 LAYERED=0,1,2
// m.ml.shade (MegaLights.hlsli): ShadeOpaque.hlsl compiled with MEGA_LIGHTS = 1 - the same tiles, classes, per-pixel setup
// and light models (punctual lights, area lights by LTC, A9 layers, Foliage, light functions), with the local-light loop
// running over the lights of the pixel's light samples instead of the froxel list and each light's visibility replaced
// by its samples' weight; no emission, sun, tile term or indirect light. It writes the lights' diffuse and specular
// radiance, exposed and divided by the modulation factors: P[10].w (RGBA16F: rgb diffuse, a = shading confidence) and
// P[11].x (RGBA16F: rgb specular, a = 1 valid / 0 no sample holds this surface).
// P[10].y = the light samples (R32G32_UINT), P[10].z = the downsampled key, P[11].y = weight caps (f16 visible | f16 hidden
// << 16), P[11].z = minimum sample weight (float), P[11].w = factor | N << 8.
#define MEGA_LIGHTS 1
#define SHADE_PART 1
#define FALLBACK 0
#define PLANAR 0
#include "Passes/Shading/ShadeOpaque.hlsl"
