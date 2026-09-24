// Public shadow lookups (INTERFACES_KO.md 5.6, 7.3). Owner: S. Consumer: M's shading kernels.
//   shadowVisibility R32_UINT: 4 slots x 8 bit unorm (0 = full shadow, 255 = fully lit). Slot 0 = sun, slots 1-3 = the
//   first three shadow-casting local lights of the pixel's froxel light list, in list order.
#ifndef UNX_SHADOW_VISIBILITY_HLSLI
#define UNX_SHADOW_VISIBILITY_HLSLI

float shadowSlot(uint packed, uint slot) { return ((packed >> (8 * slot)) & 0xFFu) / 255.0; }

#endif
