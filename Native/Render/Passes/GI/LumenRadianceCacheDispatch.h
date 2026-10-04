#ifndef UNX_LUMEN_RADIANCE_CACHE_DISPATCH_H
#define UNX_LUMEN_RADIANCE_CACHE_DISPATCH_H
// Shared CPU/HLSL layout: three independent DISPATCH_ARGUMENTS records per
// probe chunk. Irradiance has one 8x8 group per probe, independent of the
// radiance atlas resolution used by filter/store.
#define LRC_FILTER_ARGS_OFFSET 0u
#define LRC_STORE_ARGS_OFFSET 16u
#define LRC_IRRADIANCE_ARGS_OFFSET 32u
#define LRC_COMPUTE_ARGS_STRIDE 48u
#define LRC_IRRADIANCE_GROUP_AXIS(count) ((count) > 0u ? 1u : 0u)

#ifdef __cplusplus
static_assert(LRC_FILTER_ARGS_OFFSET + 12 <= LRC_STORE_ARGS_OFFSET);
static_assert(LRC_STORE_ARGS_OFFSET + 12 <= LRC_IRRADIANCE_ARGS_OFFSET);
static_assert(LRC_IRRADIANCE_ARGS_OFFSET + 12 <= LRC_COMPUTE_ARGS_STRIDE);
static_assert(LRC_IRRADIANCE_GROUP_AXIS(0) == 0);
static_assert(LRC_IRRADIANCE_GROUP_AXIS(1) == 1);
static_assert(LRC_IRRADIANCE_GROUP_AXIS(64) == 1);
#endif
#endif
