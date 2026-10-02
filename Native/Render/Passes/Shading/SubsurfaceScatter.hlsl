// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// m.sss.scatter (shading.subsurface_scatter; SubsurfaceScatter.hlsli states the passes and the estimate): ShadeOpaque.hlsl
// compiled with SHADE_PART = 3 - the Subsurface class's tiles and pixels and the per-pixel setup, then
//   colour = specular + emission (P[10].x, SubsurfaceIndirect's sum) + f_d x the scattered diffuse light (P[9].z)
// and that file's air, exposure histogram, particles, output and edge / coverage radiance, as part 2 of the plain classes
// writes them. One kernel for every view (it reads no screen probes and no light) and for the tiles over S's overflow
// capacity (P[3].z = UNX_NONE with the fallback tile list and a class mask; with S's tile heads the main run leaves those
// tiles to that run, whose diffuse light is complete only after the fallback kernels).
#define SSS_SPLIT 1
#define SHADE_PART 3
#define AREA 0
#define FALLBACK 0
#define PLANAR 0
#define LAYERED 3
#include "Passes/Shading/ShadeOpaque.hlsl"
