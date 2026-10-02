// unx-kernel: cs_6_6 main
// unx-variants: FALLBACK=0,1 PLANAR=0,1
// Subsurface class with shading.subsurface_scatter, part 2 (SubsurfaceScatter.hlsli states the passes): ShadeOpaque.hlsl's
// part 2 (ShadeIndirect.hlsl's kernel of the plain classes) compiled with SSS_SPLIT = 1 - the same indirect light, with
// the specular light and the emission written back to the direct radiance texture and the indirect diffuse light per
// unit f_d added to the class's diffuse texture P[9].z, whose alpha takes the pixel's view depth. No air and no output:
// the scatter kernel (SubsurfaceScatter.hlsl) writes them. With the switch off the class runs ShadeIndirect.LAYERED0.
#define SSS_SPLIT 1
#define SHADE_PART 2
#define AREA 0
#define OUTPUT 1
#define LAYERED 3
#include "Passes/Shading/ShadeOpaque.hlsl"
