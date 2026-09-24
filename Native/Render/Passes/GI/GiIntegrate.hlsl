// unx-kernel: cs_6_6 main
// Projects each updated entry's 64 hemispherical texels onto cosine-convolved L2 spherical harmonics:
//   local:  L_lm = sum_t L_t * integral over texel t of Y_lm (exact per-texel integrals, GiSystem's table in the cache
//           buffer; the texels are piecewise-constant radiance), E_lm = A_l L_lm, A = (pi, 2pi/3, pi/4);
//   world:  band 1 rotates as a vector, band 2 as the traceless quadratic form Q' = R Q R^T, R = [t b n] (exact).
// Keeps the sun visibility half of word 13; records the update (count, history since reset, epoch, frame).
// Threads: one per update slot (selected entries, then background). P[0] = { cache UAV, updates per frame, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    uint entry;
    bool background;
    if (!giUpdateSlot(b, h, slot, entry, background)) return;

    float3 c[9];
    [unroll] for (uint j = 0; j < 9; ++j) c[j] = 0;
    [loop] for (uint texel = 0; texel < GI_TEXEL_COUNT; ++texel)
    {
        const uint2 v = b.Load2(h.offTexels + (entry * GI_TEXEL_COUNT + texel) * 8);
        const float3 radiance = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) * GI_LOAD_SCALE;
        const uint row = h.offShTable + texel * 36;
        const float4 y0 = asfloat(b.Load4(row)), y1 = asfloat(b.Load4(row + 16));
        const float y8 = asfloat(b.Load(row + 32));
        c[0] += radiance * y0.x; c[1] += radiance * y0.y; c[2] += radiance * y0.z; c[3] += radiance * y0.w;
        c[4] += radiance * y1.x; c[5] += radiance * y1.y; c[6] += radiance * y1.z; c[7] += radiance * y1.w;
        c[8] += radiance * y8;
    }

    // Rotation local -> world: world = t * x + b * y + n * z.
    const float3 n = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(n, t, bt);
    const float3x3 R = float3x3(t.x, bt.x, n.x, t.y, bt.y, n.y, t.z, bt.z, n.z);  // columns t, b, n
    float3 w[9];
    w[0] = c[0];
    // Band 1: Y1 = (y, z, x) * 0.488603 -> coefficient vector v = (c3, c1, c2) in (x, y, z).
    [unroll] for (uint ch = 0; ch < 3; ++ch)
    {
        const float3 v = float3(c[3][ch], c[1][ch], c[2][ch]);
        const float3 vw = mul(R, v);
        w[1][ch] = vw.y;
        w[2][ch] = vw.z;
        w[3][ch] = vw.x;
        // Band 2 as a quadratic form: f = a4 xy + a5 yz + a6 (3z^2 - 1) + a7 xz + a8 (x^2 - y^2).
        const float a4 = 1.092548 * c[4][ch], a5 = 1.092548 * c[5][ch], a6 = 0.315392 * c[6][ch], a7 = 1.092548 * c[7][ch], a8 = 0.546274 * c[8][ch];
        const float3x3 Q = float3x3(a8 - a6, 0.5 * a4, 0.5 * a7, 0.5 * a4, -a8 - a6, 0.5 * a5, 0.5 * a7, 0.5 * a5, 2 * a6);
        const float3x3 Qw = mul(mul(R, Q), transpose(R));
        w[4][ch] = 2 * Qw[0][1] / 1.092548;
        w[5][ch] = 2 * Qw[1][2] / 1.092548;
        w[6][ch] = 0.5 * Qw[2][2] / 0.315392;
        w[7][ch] = 2 * Qw[0][2] / 1.092548;
        w[8][ch] = 0.5 * (Qw[0][0] - Qw[1][1]) / 0.546274;
    }
    const float a[9] = { GI_PI, 2 * GI_PI / 3, 2 * GI_PI / 3, 2 * GI_PI / 3, GI_PI / 4, GI_PI / 4, GI_PI / 4, GI_PI / 4, GI_PI / 4 };
    float v[27];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const float3 e = w[k] * (a[k] * GI_STORE_SCALE);
        v[3 * k] = e.r;
        v[3 * k + 1] = e.g;
        v[3 * k + 2] = e.b;
    }
    const uint address = h.offSh + entry * GI_SH_STRIDE;
    const uint word13 = b.Load(address + 52);
    uint word[14];
    [unroll] for (uint i = 0; i < 13; ++i) word[i] = giPackHalf2(v[2 * i], v[2 * i + 1]);
    word[13] = f32tof16(v[26]) | (word13 & 0xFFFF0000u);
    b.Store4(address, uint4(word[0], word[1], word[2], word[3]));
    b.Store4(address + 16, uint4(word[4], word[5], word[6], word[7]));
    b.Store4(address + 32, uint4(word[8], word[9], word[10], word[11]));
    b.Store2(address + 48, uint2(word[12], word[13]));
    const uint history = giHistory(b, h, entry);
    if (history == 0) b.InterlockedAdd(GI_H_STAT_RESETS, 1u);
    b.Store(address + GI_SH_UPDATES, b.Load(address + GI_SH_UPDATES) + 1);
    b.Store(address + GI_SH_LAST_UPDATE, h.frame);
    b.Store2(address + GI_SH_HISTORY, uint2(history + 1, h.epoch));
}
