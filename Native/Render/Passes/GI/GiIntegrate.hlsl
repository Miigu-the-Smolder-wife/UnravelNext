// unx-kernel: cs_6_6 main
// Folds each updated entry's 64 new ray samples (GiTrace: radiance and the ray's hemispherical octahedral coordinates)
// into its irradiance, per ray (design 2.5 revision, request 18): every sample weighs L by its own direction and solid
// angle, dw = 2 / |p|^3 x the texel's UV area (p = the octahedron point of the map; the density integrates to 2 pi), not
// the texel's average spread over the texel (a thin band of horizon light is not moved up to where cos is larger).
//   irradiance map: E(n_j) += L max(0, n_j . w) dw at the 9 x 9 directions around the anchor normal (GiCache.hlsli);
//   SH (world frame, cosine-convolved L2; the screen probes still use it): E_lm += A_l L Y_lm(w) dw.
// The texels (GiTrace's third sample block: radiance, hit distance), the map and the SH blend into the entry's history with
// one weight (giHistoryAlpha), set by the share of the update's bounce reads that came from young cells; the sun
// visibility half of SH word 13 is kept; the update is recorded (count, history word, epoch, frame).
// The emitter samples (GiTrace: one per texel ray, MIS-weighted radiance over p_l, direction in the anchor frame) follow
// the texel samples at index ray budget + slot x 64 + k; each adds L_w max(0, n_j . w) / 64.
// One group per update slot (selected entries, then background). P[0] = { cache UAV, updates per frame, samples SRV, 0 }
#include "Scene.hlsli"
#include "Passes/GI/GiInternal.hlsli"

groupshared float4 gs_sample[2 * GI_TEXEL_COUNT];  // radiance, solid-angle weight (texel samples, then emitter samples)
groupshared float3 gs_local[2 * GI_TEXEL_COUNT];   // direction in the anchor frame
groupshared float3 gs_sh[9];
groupshared float2 gs_bounce[GI_TEXEL_COUNT];  // per texel ray: 1 if its bounce read was young, 1 if it read a bounce term

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
    const uint history = giHistory(b, h, entry);
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
        // A non-finite ray value (from a bad hit: never expected) is left out, not averaged in: a texel or SH value that
        // became NaN would stay so and spread through every bounce that reads it.
        const float3 value = asfloat(s.xyz);
        const bool finite = all(value == value) && all(abs(value) < 3.0e38);
        gs_sample[lane] = finite ? float4(value, 2 / (len * len * len) / (float)GI_TEXEL_COUNT) : float4(0, 0, 0, 0);
        gs_local[lane] = q / len;
        const uint w = samples[2 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane].w;
        gs_bounce[lane] = float2((w >> 17) & 1u, (w >> 16) & 1u);
    }
    else if (lane < 2 * GI_TEXEL_COUNT)
    {
        const uint4 s = samples[P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane - GI_TEXEL_COUNT];
        const float3 value = asfloat(s.xyz);
        const bool finite = all(value == value) && all(abs(value) < 3.0e38);
        gs_sample[lane] = finite ? float4(value, 1.0 / GI_TEXEL_COUNT) : float4(0, 0, 0, 0);
        gs_local[lane] = octDecode(s.w);
    }
    GroupMemoryBarrierWithGroupSync();
    float2 bounce = 0;
    [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) bounce += gs_bounce[k];
    const float young = bounce.y > 0 ? bounce.x / bounce.y : 0.0;  // no bounce light read: nothing biased
    const float alpha = giHistoryAlpha(h, history, young);

    // Texels: lanes 0..63 (radiance, hit distance).
    if (lane < GI_TEXEL_COUNT)
    {
        const uint4 s = samples[2 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane];
        const uint address = h.offTexels + (entry * GI_TEXEL_COUNT + lane) * 8;
        const uint2 old = b.Load2(address);
        const float3 previous = float3(f16tof32(old.x), f16tof32(old.x >> 16), f16tof32(old.y)) * GI_LOAD_SCALE;
        const float3 sample = asfloat(s.xyz);
        const bool finite = all(sample == sample) && all(abs(sample) < 3.0e38);
        const float a = finite ? alpha : 0.0;  // a non-finite value keeps the texel as it was
        const float3 value = lerp(previous, finite ? sample : previous, a) * GI_STORE_SCALE;
        const float dist = lerp(f16tof32(old.y >> 16), f16tof32(s.w & 0xFFFFu), a);
        b.Store2(address, uint2(giPackHalf2(value.r, value.g), giPackHalf2(value.b, dist)));
    }

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
    word[13] = f32tof16(nearestHalf(v[26])) | (word13 & 0xFFFF0000u);
    b.Store4(address, uint4(word[0], word[1], word[2], word[3]));
    b.Store4(address + 16, uint4(word[4], word[5], word[6], word[7]));
    b.Store4(address + 32, uint4(word[8], word[9], word[10], word[11]));
    b.Store2(address + 48, uint2(word[12], word[13]));
    if (history == 0) b.InterlockedAdd(GI_H_STAT_RESETS, 1u);
    b.Store(address + GI_SH_UPDATES, b.Load(address + GI_SH_UPDATES) + 1);
    b.Store(address + GI_SH_LAST_UPDATE, h.frame);
    b.Store2(address + GI_SH_HISTORY, uint2(giHistoryNext(h, history, young), h.epoch));
}
