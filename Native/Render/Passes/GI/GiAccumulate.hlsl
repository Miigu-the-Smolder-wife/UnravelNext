// unx-kernel: cs_6_6 main
// gi.hit_accumulator (GiInternal.hlsli GI_ACC_*, RENDERER_REDESIGN_V2 12.1): after the GI rays, every entry with frame sums
// folds them into its direct-light means and clears them (all entries: the accumulator's cells need not be on the hit list). Samples-weighted running mean: the exact mean
// while the cell holds fewer than the window's samples, then the frame's batch weighs its count over the window. A mean
// of an older lighting epoch starts anew. One thread per listed cell (each writes its own entry: any order, same result).
// P[0] = { cache UAV, window (float, samples), 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint entry = id;
    if (entry >= h.capacity) return;
    const uint sums = b.Load(GI_ACC_SUMS) + entry * GI_ACC_SUMS_BYTES;
    const float n = (float)b.Load(sums + GI_ACC_COUNT);
    if (!(n > 0)) return;
    // Each term's mean: its numerator sum over its weight sum (the weights are 1 without gi.hit_accumulator_ratio: the
    // plain mean); a term no hit weighed (k_T = 0: no reader uses it) is 0.
    double s[16];
    [unroll] for (uint k = 0; k < 16; ++k)
    {
        const uint2 w = b.Load2(sums + k * 8);
        s[k] = (double)w.y * 4294967296.0 + (double)w.x;
    }
    float v[9];
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        v[c] = s[3 + c] > 0 ? (float)(s[c] / s[3 + c]) : 0.0;
        v[3 + c] = s[9 + c] > 0 ? (float)(s[6 + c] / s[9 + c]) : 0.0;
        v[6 + c] = s[15] > 0 ? (float)(s[12 + c] / s[15]) : 0.0;
    }
    [unroll] for (uint z = 0; z < GI_ACC_SUMS_BYTES / 16; ++z) b.Store4(sums + z * 16, uint4(0, 0, 0, 0));
    const uint a = b.Load(GI_ACC_OFFSET) + entry * 48;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32);
    float old[9] = { asfloat(w0.x), asfloat(w0.y), asfloat(w0.z), asfloat(w0.w), asfloat(w1.x), asfloat(w1.y), asfloat(w1.z), asfloat(w1.w), asfloat(w2.x) };
    float held = w2.z == h.epoch ? asfloat(w2.y) : 0.0;
    if ((b.Load(GI_ACC_MODE) & 1u) != 0) held = 0;  // gi.hit_accumulator_frame: this frame's means alone (GiAccFix)
    const float window = max(asfloat(P[0].y), 1.0), total = held + n;
    const float weight = total <= window ? n / total : min(n / window, 1.0);
    float o[9];
    [unroll] for (uint j = 0; j < 9; ++j) o[j] = held > 0 ? old[j] + (v[j] - old[j]) * weight : v[j];
    b.Store4(a, asuint(float4(o[0], o[1], o[2], o[3])));
    b.Store4(a + 16, asuint(float4(o[4], o[5], o[6], o[7])));
    b.Store4(a + 32, uint4(asuint(o[8]), asuint(min(total, window)), h.epoch, 0));
}
