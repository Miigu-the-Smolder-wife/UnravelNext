// unx-kernel: cs_6_6 main
// Folds each updated entry's 64 new ray samples (GiTrace: radiance and the ray's hemispherical octahedral coordinates)
// into its irradiance, per ray (design 2.5 revision, request 18): every sample weighs L by its own direction and solid
// angle, dw = 2 / |p|^3 x the texel's UV area (p = the octahedron point of the map; the density integrates to 2 pi), not
// the texel's average spread over the texel (a thin band of horizon light is not moved up to where cos is larger).
//   irradiance map: E(n_j) += L max(0, n_j . w) dw at the 9 x 9 directions around the anchor normal (GiCache.hlsli);
//   SH (world frame, cosine-convolved L2; the screen probes still use it): E_lm += A_l L Y_lm(w) dw.
// Both blend into the entry's history with the texels' weight (giHistoryAlpha); the sun visibility half of SH word 13 is
// kept; the update is recorded (count, history since reset, epoch, frame).
// The emitter samples (GiTrace: one per texel ray, MIS-weighted radiance over p_l, direction in the anchor frame) follow
// the texel samples at index ray budget + slot x 64 + k; each adds L_w max(0, n_j . w) / 64.
// One group per update slot (selected entries, then background). P[0] = { cache UAV, updates per frame, samples SRV, 0 }
#include "Scene.hlsli"
#include "Passes/GI/GiInternal.hlsli"

groupshared float4 gs_sample[2 * GI_TEXEL_COUNT];  // radiance, solid-angle weight (texel samples, then emitter samples)
groupshared float3 gs_local[2 * GI_TEXEL_COUNT];   // direction in the anchor frame
groupshared float3 gs_sh[9];

[numthreads(128, 1, 1)]
void main(uint lane : SV_GroupIndex, uint slot : SV_GroupID)
{
    if (slot >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint4> samples = ResourceDescriptorHeap[P[0].z];
    const GiHeader h = giHeader(b);
    uint entry;
    bool background;
    if (!giUpdateSlot(b, h, slot, entry, background)) return;  // uniform over the group
    const float alpha = giHistoryAlpha(h, giHistory(b, h, entry));
    const float3 na = giAnchorNormal(b, h, entry);
    float3 t, bt;
    giBasis(na, t, bt);

    if (lane < GI_TEXEL_COUNT)
    {
        const uint4 s = samples[slot * GI_TEXEL_COUNT + lane];
        const float2 uv = float2(s.w & 0xFFFFu, s.w >> 16) / 65535.0;
        // p on the octahedron (hemispherical map: a = u + v - 1, b = u - v, z = 1 - |a| - |b|); dw / (du dv) = 2 / |p|^3.
        const float2 xy = uv * 2 - 1;
        const float2 ab = float2((xy.x + xy.y) * 0.5, (xy.x - xy.y) * 0.5);
        const float3 q = float3(ab, 1 - abs(ab.x) - abs(ab.y));
        const float len = length(q);
        gs_sample[lane] = float4(asfloat(s.xyz), 2 / (len * len * len) / (float)GI_TEXEL_COUNT);
        gs_local[lane] = q / len;
    }
    else if (lane < 2 * GI_TEXEL_COUNT)
    {
        const uint4 s = samples[P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane - GI_TEXEL_COUNT];
        gs_sample[lane] = float4(asfloat(s.xyz), 1.0 / GI_TEXEL_COUNT);
        gs_local[lane] = octDecode(s.w);
    }
    GroupMemoryBarrierWithGroupSync();

    // SH: lanes 0..8, one coefficient each (three channels), in the world frame.
    if (lane < 9)
    {
        float3 c = 0;
        [loop] for (uint k = 0; k < 2 * GI_TEXEL_COUNT; ++k)
        {
            const float3 d = gs_local[k];
            float y[9];
            giShBasis(t * d.x + bt * d.y + na * d.z, y);
            c += gs_sample[k].xyz * (y[lane] * gs_sample[k].w);
        }
        const float a = lane == 0 ? GI_PI : (lane < 4 ? 2 * GI_PI / 3 : GI_PI / 4);
        gs_sh[lane] = c * a;
    }
    // Irradiance map: lanes 0..80, one direction each.
    if (lane < GI_IRR_N * GI_IRR_N)
    {
        const uint ix = lane % GI_IRR_N, iy = lane / GI_IRR_N;
        const float3 nj = giHemiOctDecode((float2(ix, iy) + 0.5) / (float)GI_IRR_N);
        float3 e = 0;
        [loop] for (uint k = 0; k < 2 * GI_TEXEL_COUNT; ++k) e += gs_sample[k].xyz * (max(dot(nj, gs_local[k]), 0.0) * gs_sample[k].w);
        const uint address = h.offIrr + entry * GI_IRR_STRIDE + lane * 4;
        const float3 previous = giIrrUnpack(b.Load(address)) * GI_LOAD_SCALE;
        b.Store(address, giPackRgb9e5(lerp(previous, e, alpha) * GI_STORE_SCALE));
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0) return;

    float3 previous[9];
    giLoadSh(b, h, entry, previous);
    float v[27];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const float3 e = lerp(previous[k], gs_sh[k], alpha) * GI_STORE_SCALE;
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
