// unx-kernel: cs_6_6 main
// Coverage composite, compact form, stage S in one kernel (CoverageShadeList.hlsli PART 0): every listed fragment's
// direct and indirect light in one shading - one surface, material and air evaluation per fragment where the two-part
// form makes two. Without the area lights' loop (AREA 0: 176 KB): frames without area lights, and frames whose
// fragments take the local lights from the coverage MegaLights instance (the loop is not run).
#define PART 0
#define AREA 0
#include "Passes/Shading/CoverageShadeList.hlsli"
