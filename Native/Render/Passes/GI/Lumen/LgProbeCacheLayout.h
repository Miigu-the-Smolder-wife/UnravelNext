#ifndef UNX_LG_PROBE_CACHE_LAYOUT_H
#define UNX_LG_PROBE_CACHE_LAYOUT_H
// Internal, frame-local ABI. No quantization: header, integer cell corner, then
// eight {probe slot, FP32 trilinear weight} pairs in the reference lookup order.
#define LG_PROBE_CACHE_BYTES 96u
#define LG_PROBE_CACHE_PREPARED 0x20000u
#define LG_PROBE_CACHE_NEAR_FIRST 0x10u
#endif
