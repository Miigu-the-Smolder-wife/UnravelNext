// unx-kernel: cs_6_6 main
// unx-variants: FALLBACK=0,1 PLANAR=0,1 LAYERED=1,2
// A9 area-light lobes (render A): the terms of lobes no LTC represents - the anisotropic base (MATERIAL_LAYERS 1.5) and
// the sheen (1.4) - over each area light, by AreaQuadrature.hlsli. It is ShadeOpaque.hlsl compiled with AREA_LOBES = 1:
// the same tiles, classes, light lists and visibility (S's slots, the overflow list, or S's VSM in fallback tiles), no
// other term; it writes the exposed radiance to P[9].z, which the LAYERED ShadeOpaque variants add. Split from ShadeOpaque
// because the quadrature inlined there puts the layered variants at 201-248 KB of DXIL (limit 200 KB).
#define AREA_LOBES 1
#define AREA 1
#define OUTPUT 0
#include "Passes/Shading/ShadeOpaque.hlsl"
