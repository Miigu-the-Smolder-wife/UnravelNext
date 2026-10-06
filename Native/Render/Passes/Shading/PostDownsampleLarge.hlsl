// unx-kernel: cs_6_6 main
// The same filter with fewer overlapping local-exposure evaluations per output.
#define POST_TILE_SIZE 16
#include "Passes/Shading/PostDownsample.hlsl"
