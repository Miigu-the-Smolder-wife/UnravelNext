// unx-kernel: cs_6_6 main
// m.ml.spatial.sss (shading.mega_lights with shading.subsurface_scatter): MegaLightsSpatial.hlsl compiled with
// ML_SPATIAL_SUBSURFACE = 1 - the spatial step of the stochastic local lights again on the Subsurface class's tiles, for
// that class's pixels, with the diffuse and the specular apart (m.ml.spatial writes their sum, which the plain classes
// add). The diffuse, per unit f_d, starts the class's diffuse texture for the scatter pass; the specular is what the
// class's part 2 adds in place of the sum (SubsurfaceScatter.hlsli states the passes). The same inputs, constants and
// arithmetic as m.ml.spatial up to the factors.
#define ML_SPATIAL_SUBSURFACE 1
#include "Passes/Shading/MegaLightsSpatial.hlsl"
