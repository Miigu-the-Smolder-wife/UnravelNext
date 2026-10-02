// unx-kernel: cs_6_6 main
// unx-variants: FALLBACK=0,1 AREA=0,1 PLANAR=0,1
// Subsurface class with shading.subsurface_scatter, part 1 (SubsurfaceScatter.hlsli states the passes): ShadeOpaque.hlsl's
// Subsurface variant (LAYERED = 3: the two specular lobes, the light through thin parts) compiled with SSS_SPLIT = 1 - the
// same tiles, pixels, lights and visibility; the direct radiance texture takes the specular light and the emission alone,
// and the diffuse light per unit f_d goes to the class's diffuse texture P[9].z for the scatter pass. With the switch off
// the class runs ShadeOpaque.LAYERED3 as before.
#define SSS_SPLIT 1
#define LAYERED 3
#include "Passes/Shading/ShadeOpaque.hlsl"
