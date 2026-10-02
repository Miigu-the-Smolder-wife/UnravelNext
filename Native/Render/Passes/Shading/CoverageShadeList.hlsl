// unx-kernel: cs_6_6 main
// unx-variants: PART=1,2 AREA=0,1
// Coverage composite, compact form, stage S in two parts (CoverageShadeList.hlsli): the direct light, then the indirect
// light - for frames whose fragments run the area lights' loop (AREA=1 without the coverage MegaLights instance): the
// one-kernel form with that loop stands at the DXIL limit (202,580 B of 204,800).
#include "Passes/Shading/CoverageShadeList.hlsli"
