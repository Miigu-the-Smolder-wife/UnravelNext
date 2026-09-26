#pragma once
// Round to the nearest half (ties to even) before a store to a 16-bit float target. This GPU's float -> half conversion
// (typed UAV stores and f32tof16 alike) rounds toward zero: a mean bias of about -2.4e-4 per store, which a chain of
// fp16 passes accumulates (the bloom pyramid lost 2.2e-3 of an impulse's energy over 6 levels; PostTests, [실측]). The
// returned value is exactly representable in half, so the store keeps it whatever the hardware's rounding.
float halfRound(float x)
{
    const uint h = f32tof16(x);  // the toward-zero neighbour
    const float lo = f16tof32(h);
    if (lo == x || (h & 0x7FFFu) >= 0x7BFFu) return lo;  // exact, or at the largest finite half (overflow left to the store)
    const float hi = f16tof32(h + 1);  // the next half away from zero (sign-magnitude)
    const float dl = abs(x - lo), dh = abs(hi - x);
    return (dh < dl || (dh == dl && (h & 1u) != 0)) ? hi : lo;
}
float3 halfRound(float3 v) { return float3(halfRound(v.x), halfRound(v.y), halfRound(v.z)); }
