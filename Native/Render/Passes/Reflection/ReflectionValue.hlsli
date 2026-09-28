// The split and overflow paths share every storage boundary, including the
// rounding before/after deferred sun visibility. Keep the arithmetic here.
#ifndef UNX_REFLECTION_VALUE_HLSLI
#define UNX_REFLECTION_VALUE_HLSLI
#include "Passes/Reflection/ReflectionInternal.hlsli"

uint reflPackHalf2(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }
uint reflPackBarycentrics(float2 b)
{
    const uint2 q = uint2(round(saturate(b) * 65535.0));
    return q.x | (q.y << 16);
}
float2 reflUnpackBarycentrics(uint v) { return float2(v & 0xFFFFu, v >> 16) / 65535.0; }

uint4 reflStoreValue(float3 radiance, float3 sun, float distanceToHit, float motion)
{
    const float3 r = radiance * REFL_STORE_SCALE, s = sun * REFL_STORE_SCALE;
    return uint4(reflPackHalf2(r.r, r.g), reflPackHalf2(r.b, min(distanceToHit, 65000.0)), reflPackHalf2(s.r, s.g),
                 f32tof16(s.b) | (1u << 16) | ((f32tof16(min(motion, 60000.0)) & 0x7FFFu) << 17));
}
uint4 reflResolveSunValue(uint4 v, float visibility)
{
    // precise forbids contracting/reassociating this storage-boundary operation
    // differently in a ray-generation library and a compute shader.
    precise float3 radiance = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) +
                             visibility * float3(f16tof32(v.z), f16tof32(v.z >> 16), f16tof32(v.w));
    return uint4(reflPackHalf2(radiance.r, radiance.g), f32tof16(radiance.b) | (v.y & 0xFFFF0000u), 0, v.w & 0xFFFF0000u);
}
float3 reflValueRadiance(uint4 v)
{
    return float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) / REFL_STORE_SCALE;
}
#endif
