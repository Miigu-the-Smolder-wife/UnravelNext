// unx-kernel: cs_6_6 main
// Round basins, per evolution: one thread per mode: the complex amplitudes (H, Phi) take this frame's increment (P[4].z
// bit 0, RoundAnalysis), then the exact rotation by w dt (Ripple's, normalised: the gain is the damping alone) and the
// damping exp(-delta dt) (RoundPool.cpp roundTables, double). Mode 0 of order 0 (k = 0, the mean level) keeps H and
// drops Phi (no restoring force), as the rectangular basin's.
#include "Passes/Water/RoundPool.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint4 last = roundOrder(ROUND_ORDERS - 1);
    if (i >= last.x + last.y) return;
    RWByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    float4 hp = asfloat(modes.Load4(16 * i));  // (H re, H im, Phi re, Phi im)
    if (P[4].z & 1u)
    {
        ByteAddressBuffer increments = ResourceDescriptorHeap[P[0].y];
        hp += asfloat(increments.Load4(16 * i));
    }
    ByteAddressBuffer table = ResourceDescriptorHeap[P[3].x];
    const float4 e = asfloat(table.Load4(16 * i));  // w, K / w, w / K, delta
    float2 H = hp.xy, Phi = hp.zw;
    if (e.x > 0)
    {
        const float dt = asfloat(P[1].x);
        float s, c;
        roundSinCos(e.x * dt, s, c);
        const float unit = rsqrt(c * c + s * s);
        c *= unit;
        s *= unit;
        const float decay = exp(-e.w * dt);
        const float2 h2 = (H * c + Phi * (e.y * s)) * decay, p2 = (Phi * c - H * (e.z * s)) * decay;
        H = h2;
        Phi = p2;
    }
    else Phi = 0;
    modes.Store4(16 * i, asuint(float4(H, Phi)));
}
