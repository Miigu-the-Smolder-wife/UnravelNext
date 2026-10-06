// unx-kernel: cs_6_6 main
// unx-variants: AREA=0,1 PLANAR=0,1 OUTPUT=0,1
// Standard/Foliage with a valid MegaLights result: direct, indirect and output
// in one dispatch. The complete local-light loop is already evaluated by ML;
// layered, subsurface and overflow tiles retain their existing split paths.
#define SHADE_PART 4
#define FALLBACK 0
#define LAYERED 0
#include "Passes/Shading/ShadeOpaque.hlsl"
