#ifndef UNX_RT_HALF_NEAREST_HLSLI
#define UNX_RT_HALF_NEAREST_HLSLI
// Round to the nearest half (ties to even) before a 16-bit float store, for R's running means (GI cache texels and SH,
// reflection accumulation). This GPU's float -> half conversion (f32tof16 and typed UAV stores) truncates toward zero, a
// mean bias of about -2.4e-4 per store [measured by M, PostTests]; a running mean of weight alpha carries it as about
// -2.4e-4 / alpha in its steady state (-0.8 % at 1/32). The same method as M's HalfRound.hlsli (its own name here, so
// kernels may include both). The result is exactly representable, so the store keeps it whatever the hardware rounds.
float nearestHalf(float x)
{
    const uint h = f32tof16(x);  // the toward-zero neighbour
    const float lo = f16tof32(h);
    if (lo == x || (h & 0x7FFFu) >= 0x7BFFu) return lo;  // exact, or at the largest finite half (overflow left to the store)
    const float hi = f16tof32(h + 1);  // the next half away from zero (sign-magnitude)
    const float dl = abs(x - lo), dh = abs(hi - x);
    return (dh < dl || (dh == dl && (h & 1u) != 0)) ? hi : lo;
}
float3 nearestHalf(float3 v) { return float3(nearestHalf(v.x), nearestHalf(v.y), nearestHalf(v.z)); }
#endif
