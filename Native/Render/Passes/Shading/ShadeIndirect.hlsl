// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1 FALLBACK=0,1 PLANAR=0,1 LAYERED=0,1,2
// Shading of the opaque classes, part 2 (ShadeOpaque.hlsl compiled with SHADE_PART = 2; see its header): the same tiles,
// classes and per-pixel setup, then R's indirect light (screen probes or the world cache), the A9 lobe texture, the air
// between the camera and the surface, and the output (exposure histogram, particles, edge / coverage radiance). It
// starts from the direct radiance part 1 wrote (P[10].x, RGBA32F): the one kernel's sum continued bit for bit. Split
// from ShadeOpaque on 2026-10-01 because the one kernel's FALLBACK variants stood at 204,260 of the 204,800 B DXIL
// limit (the fallback variants are kept here only for the fallback tile list's dispatch; they hold no VSM code).
#define SHADE_PART 2
#define AREA 0
#include "Passes/Shading/ShadeOpaque.hlsl"
