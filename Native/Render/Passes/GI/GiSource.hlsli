// The indirect-light source of the passes that light what is not an opaque surface of the view - air and fog, particles,
// water, glass. One word, in the place where those passes took the world GI cache's SRV:
//   UNX_NONE                 none
//   GI_SOURCE_VOLUME | srv   the Lumen translucency volume's parameters (LumenTranslucencyVolume.hlsli) - the source
//                            whenever the frame published the volume (lumen.translucency_volume), and the only one
//                            under gi.lumen_only
//   srv                      the world GI cache (raw)
// C++: unx/render/Frame.h GiSource, giSource, declareGiSource, giSourceWord.
#ifndef UNX_GI_SOURCE_HLSLI
#define UNX_GI_SOURCE_HLSLI
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

#define GI_SOURCE_VOLUME 0x80000000u
bool giSourceIsVolume(uint word) { return word != 0xFFFFFFFFu && (word & GI_SOURCE_VOLUME) != 0; }
bool giSourceIsCache(uint word) { return word != 0xFFFFFFFFu && (word & GI_SOURCE_VOLUME) == 0; }
uint giSourceVolume(uint word) { return word & ~GI_SOURCE_VOLUME; }

#endif
