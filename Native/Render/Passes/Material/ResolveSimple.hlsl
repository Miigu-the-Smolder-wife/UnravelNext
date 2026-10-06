// unx-kernel: cs_6_6 main
// unx-variants: DEBUG=0,1 PLANAR_MASK=0,1
// The CPU confirms that no current material uses cut/terrain/eye or height
// mapping. Textures, UV/detail inputs, anisotropy, layers and decals keep the
// same implementation. Material edits select the full kernel again.
#define RESOLVE_SIMPLE 1
#include "Passes/Material/Resolve.hlsl"
