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
groupshared float gs_shWeight[2 * GI_TEXEL_COUNT * 9];  // per sample: its SH basis x its weight (the SH lanes' factors)
groupshared float4 gs_sums;  // the group's serial sums (one wave): bounce young, bounce reads, bounce irradiance, total E
groupshared float gs_sumE2;
groupshared float2 gs_bounce[GI_TEXEL_COUNT];  // per texel ray: 1 if its bounce read was young, 1 if it read a bounce term
groupshared float gs_bounceIrradiance[GI_TEXEL_COUNT];  // per texel ray: its bounce light's share of the anchor's irradiance (luminance)

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
        const uint4 texel = samples[2 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane];
        const uint w = texel.w;
        gs_bounce[lane] = float2((w >> 17) & 1u, (w >> 16) & 1u);
        const float lum = dot(asfloat(texel.xyz), float3(0.2126, 0.7152, 0.0722));
        gs_bounceIrradiance[lane] = finite && lum < 3.0e38 ? ((w >> 18) & 4095u) / 4095.0 * lum * gs_sample[lane].w * gs_local[lane].z : 0.0;
    }
    else if (lane < 2 * GI_TEXEL_COUNT)
    {
        const uint4 s = samples[P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane - GI_TEXEL_COUNT];
        const float3 value = asfloat(s.xyz);
        const bool finite = all(value == value) && all(abs(value) < 3.0e38);
        gs_sample[lane] = finite ? float4(value, 1.0 / GI_TEXEL_COUNT) : float4(0, 0, 0, 0);
        gs_local[lane] = octDecode(s.w);
    }
    if (lane < 2 * GI_TEXEL_COUNT)
    {
        // The SH lanes' factor of this sample, y_j(w) x dw (world frame), once per sample instead of once per sample
        // and coefficient (the same products).
        const float3 d = gs_local[lane];
        float y[9];
        giShBasis(t * d.x + bt * d.y + na * d.z, y);
        const float w = gs_sample[lane].w;
        [unroll] for (uint j = 0; j < 9; ++j) gs_shWeight[lane * 9 + j] = y[j] * w;
    }
    GroupMemoryBarrierWithGroupSync();
    // The serial sums over the samples: one wave (every lane of it the same loops in the same order), shared - every wave
    // of the group ran them before.
    if (lane < WaveGetLaneCount())
    {
        float2 bounceSum = 0;
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) bounceSum += gs_bounce[k];
        float bounceSumE = 0, sumE = 0, sumE2 = 0;
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) bounceSumE += gs_bounceIrradiance[k];
        [loop] for (uint k = 0; k < 2 * GI_TEXEL_COUNT; ++k)
        {
            const float c = dot(gs_sample[k].xyz, float3(0.2126, 0.7152, 0.0722)) * gs_sample[k].w * max(gs_local[k].z, 0.0);
            sumE += c;
            sumE2 += c * c;
        }
        if (lane == 0)
        {
            gs_sums = float4(bounceSum, bounceSumE, sumE);
            gs_sumE2 = sumE2;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const float2 bounce = gs_sums.xy;
    const float young = bounce.y > 0 ? bounce.x / bounce.y : 0.0;  // no bounce light read: nothing biased
    // Jacobi length (GiInternal giJacobiLength): s = bounce share of this update's irradiance at the anchor normal; J with
    // s^J <= GI_JACOBI_RESIDUAL. Early iterates read a darker cache (s too small), so J grows during the Jacobi phase; after
    // it J is kept until a reset (a maximum over later noisy updates kept growing and sent converged cells back to Jacobi
    // replacement: the bathhouse's level drifted by +-6 % over 100 frames).
    const float bounceE = gs_sums.z, totalE = gs_sums.w, totalE2 = gs_sumE2;
    const float share = totalE > 0 ? saturate(bounceE / totalE) : 0.0;
    const uint wanted = share > 0 ? (uint)min(ceil(log(GI_JACOBI_RESIDUAL) / log(min(share, 0.9995))), 4095.0) : 0u;
    const uint storedJacobi = giHistoryJacobi(history);
    const uint jacobiLength = min(history == 0 ? max(wanted, h.jacobiUpdates)
                                  : giHistoryPhase(history) < max(storedJacobi, h.jacobiUpdates) ? max(max(storedJacobi, wanted), h.jacobiUpdates)
                                                                                                 : max(storedJacobi, h.jacobiUpdates), 4080u);
    // Window of the running mean (gi.history_updates_max_static): E = an update's anchor irradiance (luminance, the map's
    // pole), F = a fast mean of E (1/16), D = a mean of E's per-update sample spread (sigma_E, from the rays; conservative
    // for the stratified texel rays), H = the history's pole. While F stays within 3 sigma_F of H (sigma_F = D / sqrt(31))
    // the light is steady and the window grows to historyStatic; beyond, it is cut to historyMax (a light change of any
    // speed is followed as before). The test reads the state before this update: a window chosen by this update's own
    // value or spread would weight its high and low samples differently (a biased mean).
    const uint window = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_WINDOW);
    const float fastOld = f16tof32(window) * GI_LOAD_SCALE, spreadOld = f16tof32(window >> 16) * GI_LOAD_SCALE;
    const float held = dot(giIrrUnpack(b.Load(h.offIrr + entry * GI_IRR_STRIDE + GI_IRR_POLE * 4)) * GI_LOAD_SCALE, float3(0.2126, 0.7152, 0.0722));
    const bool steady = history != 0 && abs(fastOld - held) <= 3 * spreadOld / sqrt(31.0) + 0.01 * held;
    const float spread = sqrt(max(totalE2 - totalE * totalE / (2.0 * GI_TEXEL_COUNT), 0.0));
    const float fast = history == 0 ? totalE : lerp(fastOld, totalE, 1.0 / 16.0);
    const float spreadMean = history == 0 ? spread : lerp(spreadOld, spread, 1.0 / 16.0);
    const uint cap = steady ? h.historyStatic : h.historyMax;
    const float alpha = giHistoryAlpha(h, history, young, jacobiLength, cap);
    // Stochastic rounding of the stores (giDitherHalf). gi.deterministic (header flags bit 0): seeded from the entry's key
    // (giDetPriority, as GiTrace's rays), not its index - the index is the order the free list was popped in, which the
    // threads creating entries race for, so two runs of the same frames rounded differently and the cache (and every
    // image reading it) was not bit-identical (HostMotion's static shutter check failed on main).
    const uint identity = (h.flags & 1u) != 0 ? giDetPriority(b, h, entry) : entry;
    const uint seed = identity * 0x9E3779B9u ^ h.frame * 0x85EBCA6Bu;

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
        float3 value = lerp(previous, finite ? sample : previous, a) * GI_STORE_SCALE;
        [unroll] for (uint c = 0; c < 3; ++c) value[c] = giDitherHalf(value[c], giDitherUnit(seed + lane * 4 + c));
        const float dist = lerp(f16tof32(old.y >> 16), f16tof32(s.w & 0xFFFFu), a);
        b.Store2(address, uint2(giPackHalf2(value.r, value.g), giPackHalf2(value.b, dist)));
        // the emitter texel (fourth samples block, GiTrace): same running mean
        const float3 emitted = asfloat(samples[3 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane].xyz);
        const uint emitAddress = giEmitterOffset(h) + (entry * GI_TEXEL_COUNT + lane) * 4;
        const float3 emitPrevious = giIrrUnpack(b.Load(emitAddress)) * GI_LOAD_SCALE;
        const bool emitFinite = all(emitted == emitted) && all(abs(emitted) < 3.0e38);
        const uint re = seed + 4096 + lane * 4;
        b.Store(emitAddress, giPackRgb9e5(lerp(emitPrevious, emitFinite ? emitted : emitPrevious, emitFinite ? alpha : 0.0) * GI_STORE_SCALE,
                                          float3(giDitherUnit(re), giDitherUnit(re + 1), giDitherUnit(re + 2))));
    }

    // SH: lanes 96..104, one coefficient each (three channels), in the world frame, from the per-sample factors. (The last wave's lanes: the map below
    // takes lanes 0..80, so the two loops run on different waves at the same time instead of one after the other on the
    // first; the same loop per coefficient, bit-identical.)
    const uint shLane = lane - 96u;
    if (lane >= 96u && shLane < 9)
    {
        float3 c = 0;
        [loop] for (uint k = 0; k < 2 * GI_TEXEL_COUNT; ++k) c += gs_sample[k].xyz * gs_shWeight[k * 9 + shLane];
        const float a = shLane == 0 ? GI_PI : (shLane < 4 ? 2 * GI_PI / 3 : GI_PI / 4);
        gs_sh[shLane] = c * a;
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
        const uint r = seed + 1024 + lane * 4;
        b.Store(address, giPackRgb9e5(lerp(previous, e, alpha) * GI_STORE_SCALE, float3(giDitherUnit(r), giDitherUnit(r + 1), giDitherUnit(r + 2))));
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0) return;

    float3 previous[9];
    giLoadSh(b, h, entry, previous);
    float v[27];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const float3 e = lerp(previous[k], gs_sh[k], alpha) * GI_STORE_SCALE;
        v[3 * k] = giDitherHalf(e.r, giDitherUnit(seed + 2048 + 3 * k));
        v[3 * k + 1] = giDitherHalf(e.g, giDitherUnit(seed + 2049 + 3 * k));
        v[3 * k + 2] = giDitherHalf(e.b, giDitherUnit(seed + 2050 + 3 * k));
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
    b.Store3(address + GI_SH_HISTORY, uint3(giHistoryNext(h, history, young, jacobiLength, cap), h.epoch,
                                            f32tof16(nearestHalf(fast * GI_STORE_SCALE)) | (f32tof16(nearestHalf(spreadMean * GI_STORE_SCALE)) << 16)));
}
