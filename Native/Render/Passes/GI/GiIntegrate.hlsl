// unx-kernel: cs_6_6 main
// unx-variants: SPLIT=0,1
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
// Redesign V2 P1 (RENDERER_REDESIGN_V2 1.1; SPLIT=0 only):
//   a seeded entry (GiPrior wrote its converged parent's map, texels and SH into it this frame: GI_SH_RESTART bit 16, no
//     history and no update yet) weighs the seed as kappa updates: E = (E_meas + kappa E_seed) / (1 + kappa),
//     kappa = (sigma_update / delta)^2 in [2, 64] (sigma_update: this update's relative spread; delta^2: GiBegin's running
//     estimate of the parent-child difference of converged cells), then the running mean from kappa + 1 samples; the seed
//     is a converged multi-bounce value: the Jacobi phase is skipped;
//   relight restart (gi.relight_restart, off by default): an entry with 4 or more updates whose update's anchor irradiance
//     lies more than 5 sigma + 5 % from its history weighs this update 1/2 and restarts its mean, window and count;
//   the restart count (GI_SH_RESTART low 16 bits): measured updates since creation or the last restart (the tiers' order).
// One group per update slot (selected entries, then background). P[0] = { cache UAV, updates per frame, samples SRV, 0 }
#include "Scene.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#if SPLIT
#include "Passes/GI/GiSplitHistory.hlsli"
groupshared float3 gs_direct[2 * GI_TEXEL_COUNT], gs_reflected[2 * GI_TEXEL_COUNT];
groupshared float3 gs_directSh[9], gs_reflectedSh[9];
#endif

groupshared float4 gs_sample[2 * GI_TEXEL_COUNT];  // radiance, solid-angle weight (texel samples, then emitter samples)
groupshared float3 gs_local[2 * GI_TEXEL_COUNT];   // direction in the anchor frame
groupshared float3 gs_sh[9];
groupshared float gs_shWeight[2 * GI_TEXEL_COUNT * 9];  // per sample: its SH basis x its weight (the SH lanes' factors)
groupshared float3 gs_bounceSums;  // bounce young, bounce reads, bounce irradiance (one wave's serial sums)
groupshared float2 gs_totals;      // total E, total E^2 (another wave's serial sums)
groupshared float2 gs_bounce[GI_TEXEL_COUNT];  // per texel ray: 1 if its bounce read was young, 1 if it read a bounce term
groupshared float gs_bounceIrradiance[GI_TEXEL_COUNT];  // per texel ray: its bounce light's share of the anchor's irradiance (luminance)
groupshared uint gs_emitted[GI_TEXEL_COUNT];  // per emitter sample: 1 when its value is not 0

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
#if SPLIT
    const uint p1 = 0;  // the split history (experimental) keeps the previous rules
#else
    const uint p1 = b.Load(GI_P1_FLAGS);
#endif
    const uint restartWord = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_RESTART);
    const uint restartCount = giRestartUpdates(b, h, entry);
    const bool prior = (p1 & 2u) != 0 && (restartWord & 0x10000u) != 0 && history == 0 && b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) == 0;
    float3 t, bt;
    giBasis(na, t, bt);
    // The entry's stored values the update blends with, loaded before the barriers (only this group writes the entry in
    // this pass): the steady test's window and pole, each texel lane's texel and emitter texel, each map lane's direction.
    const uint window = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_WINDOW);
    const uint heldWord = b.Load(h.offIrr + entry * GI_IRR_STRIDE + GI_IRR_POLE * 4);
    const uint texelAddress = h.offTexels + (entry * GI_TEXEL_COUNT + lane) * 8;
    const uint emitAddress = giEmitterOffset(h) + (entry * GI_TEXEL_COUNT + lane) * 4;
    const uint irrAddress = h.offIrr + entry * GI_IRR_STRIDE + lane * 4;
    uint2 texelOld = 0;
    uint emitOld = 0, irrOld = 0;
    uint4 texelSample = 0;
    float3 emitted = 0;
#if SPLIT
    float3 directTexel = 0, reflectedTexel = 0;
    const uint splitBase = giSplitBase(b, entry);
    const uint splitUpdates = history == 0 ? 0 : b.Load(splitBase);
#endif

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
#if SPLIT
        const uint budget = P[0].y * GI_TEXEL_COUNT, ray = slot * GI_TEXEL_COUNT + lane;
        const float3 directSample = asfloat(samples[4 * budget + ray].xyz);
        directTexel = asfloat(samples[5 * budget + ray].xyz);
        reflectedTexel = asfloat(samples[6 * budget + ray].xyz);
        gs_direct[lane] = finite && all(isfinite(directSample)) ? directSample : float3(0, 0, 0);
        gs_reflected[lane] = finite && all(isfinite(reflectedTexel)) ? reflectedTexel : float3(0, 0, 0);
#endif
        texelSample = samples[2 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane];
        const uint w = texelSample.w;
        gs_bounce[lane] = float2((w >> 17) & 1u, (w >> 16) & 1u);
        const float lum = dot(asfloat(texelSample.xyz), float3(0.2126, 0.7152, 0.0722));
        gs_bounceIrradiance[lane] = finite && lum < 3.0e38 ? ((w >> 18) & 4095u) / 4095.0 * lum * gs_sample[lane].w * gs_local[lane].z : 0.0;
        texelOld = b.Load2(texelAddress);
        emitted = asfloat(samples[3 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane].xyz);
        emitOld = b.Load(emitAddress);
    }
    else if (lane < 2 * GI_TEXEL_COUNT)
    {
        const uint4 s = samples[P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane - GI_TEXEL_COUNT];
        const float3 value = asfloat(s.xyz);
        const bool finite = all(value == value) && all(abs(value) < 3.0e38);
        gs_sample[lane] = finite ? float4(value, 1.0 / GI_TEXEL_COUNT) : float4(0, 0, 0, 0);
        gs_local[lane] = octDecode(s.w);
        gs_emitted[lane - GI_TEXEL_COUNT] = finite && any(value != 0) ? 1u : 0u;
#if SPLIT
        gs_direct[lane] = gs_sample[lane].xyz;
        gs_reflected[lane] = 0;
#endif
    }
    if (lane < GI_IRR_N * GI_IRR_N) irrOld = b.Load(irrAddress);
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
    // The loops run over the texel samples, then the emitter samples unless all of these are 0 (no emissive triangles,
    // or none visible from the anchor): every term they would add is then +0, which leaves each sum as it is (a sum that
    // starts at +0 never becomes -0), so the sums are the full loops' bit for bit.
    bool anyEmitted = false;
    for (uint i = WaveGetLaneIndex(); i < GI_TEXEL_COUNT; i += WaveGetLaneCount()) anyEmitted = anyEmitted || gs_emitted[i] != 0;
    const uint count = WaveActiveAnyTrue(anyEmitted) ? 2 * GI_TEXEL_COUNT : GI_TEXEL_COUNT;
    // Every sum below is one loop in sample order, as before; the loops are spread over the group's waves so they run at
    // the same time (the wave holding lane 64: the map's last directions and the E sums; the wave holding lane 96: the SH
    // coefficients and the bounce sums; with 64-lane waves one wave holds both): the same values, a shorter critical path
    // than the serial sums on one wave ahead of the map and SH loops.
    const uint waveSize = WaveGetLaneCount();
    if (lane / waveSize == 96u / waveSize)
    {
        float2 bounceSum = 0;
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) bounceSum += gs_bounce[k];
        float bounceSumE = 0;
        [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) bounceSumE += gs_bounceIrradiance[k];
        if (WaveIsFirstLane()) gs_bounceSums = float3(bounceSum, bounceSumE);
    }
    if (lane / waveSize == 64u / waveSize)
    {
        float sumE = 0, sumE2 = 0;
        [loop] for (uint k = 0; k < count; ++k)
        {
            const float c = dot(gs_sample[k].xyz, float3(0.2126, 0.7152, 0.0722)) * gs_sample[k].w * max(gs_local[k].z, 0.0);
            sumE += c;
            sumE2 += c * c;
        }
        if (WaveIsFirstLane()) gs_totals = float2(sumE, sumE2);
    }
    // SH: lanes 96..104, one coefficient each (three channels), in the world frame, from the per-sample factors.
    const uint shLane = lane - 96u;
    if (lane >= 96u && shLane < 9)
    {
        float3 c = 0;
        [loop] for (uint k = 0; k < count; ++k) c += gs_sample[k].xyz * gs_shWeight[k * 9 + shLane];
        const float a = shLane == 0 ? GI_PI : (shLane < 4 ? 2 * GI_PI / 3 : GI_PI / 4);
        gs_sh[shLane] = c * a;
#if SPLIT
        float3 direct = 0, reflected = 0;
        [loop] for (uint k = 0; k < count; ++k)
        {
            direct += gs_direct[k] * gs_shWeight[k * 9 + shLane];
            reflected += gs_reflected[k] * gs_shWeight[k * 9 + shLane];
        }
        gs_directSh[shLane] = direct * a;
        gs_reflectedSh[shLane] = reflected * a;
#endif
    }
    // Irradiance map: lanes 0..80, one direction each (the sum here, the blend after the weight below).
    float3 e = 0;
#if SPLIT
    float3 directE = 0, reflectedE = 0;
#endif
    if (lane < GI_IRR_N * GI_IRR_N)
    {
        const uint ix = lane % GI_IRR_N, iy = lane / GI_IRR_N;
        const float3 nj = giHemiOctDecode((float2(ix, iy) + 0.5) / (float)GI_IRR_N);
        [loop] for (uint k = 0; k < count; ++k) e += gs_sample[k].xyz * (max(dot(nj, gs_local[k]), 0.0) * gs_sample[k].w);
#if SPLIT
        [loop] for (uint k = 0; k < count; ++k)
        {
            const float factor = max(dot(nj, gs_local[k]), 0.0) * gs_sample[k].w;
            directE += gs_direct[k] * factor;
            reflectedE += gs_reflected[k] * factor;
        }
#endif
    }
    GroupMemoryBarrierWithGroupSync();
    const float2 bounce = gs_bounceSums.xy;
    const float young = bounce.y > 0 ? bounce.x / bounce.y : 0.0;  // no bounce light read: nothing biased
    // Jacobi length (GiInternal giJacobiLength): s = bounce share of this update's irradiance at the anchor normal; J with
    // s^J <= GI_JACOBI_RESIDUAL. Early iterates read a darker cache (s too small), so J grows during the Jacobi phase; after
    // it J is kept until a reset (a maximum over later noisy updates kept growing and sent converged cells back to Jacobi
    // replacement: the bathhouse's level drifted by +-6 % over 100 frames).
    const float bounceE = gs_bounceSums.z, totalE = gs_totals.x, totalE2 = gs_totals.y;
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
    const float fastOld = f16tof32(window) * GI_LOAD_SCALE, spreadOld = f16tof32(window >> 16) * GI_LOAD_SCALE;
    const float held = dot(giIrrUnpack(heldWord) * GI_LOAD_SCALE, float3(0.2126, 0.7152, 0.0722));
    const bool steady = history != 0 && abs(fastOld - held) <= 3 * spreadOld / sqrt(31.0) + 0.01 * held;
    const float spread = sqrt(max(totalE2 - totalE * totalE / (2.0 * GI_TEXEL_COUNT), 0.0));
    const bool windowReset = history == 0 || ((p1 & 4u) != 0 && history != 0 && restartCount >= 4 && abs(totalE - held) > 5.0 * spreadOld + 0.05 * held);
    const float fast = windowReset ? totalE : lerp(fastOld, totalE, 1.0 / 16.0);
    const float spreadMean = windowReset ? spread : lerp(spreadOld, spread, 1.0 / 16.0);
    const uint cap = steady ? h.historyStatic : h.historyMax;
    const float alphaHistory = giHistoryAlpha(h, history, young, jacobiLength, cap);
    const bool restart = (p1 & 4u) != 0 && history != 0 && restartCount >= 4 && abs(totalE - held) > 5.0 * spreadOld + 0.05 * held;
    const float relSpread = totalE > 0 ? spread / totalE : 1.0;
    const float kappa = clamp(relSpread * relSpread / max(asfloat(b.Load(GI_P1_DELTA2)), 1e-6), 2.0, 64.0);
    const float alpha = prior ? 1.0 / (1.0 + kappa) : (restart ? 0.5 : alphaHistory);
#if SPLIT
    const float directAlpha = max(1.0 / (float)(splitUpdates + 1), 1.0 / (float)cap);
    const float bounceAlpha = giHistoryPhase(history) < jacobiLength ? 1.0 :
                             max(1.0 / (float)(splitUpdates + 1), 1.0 / (float)b.Load(776));
#endif
    // Stochastic rounding of the stores (giDitherHalf). gi.deterministic (header flags bit 0): seeded from the entry's key
    // (giDetPriority, as GiTrace's rays), not its index - the index is the order the free list was popped in, which the
    // threads creating entries race for, so two runs of the same frames rounded differently and the cache (and every
    // image reading it) was not bit-identical (HostMotion's static shutter check failed on main).
    const uint identity = (h.flags & 1u) != 0 ? giDetKey(b, h, entry) : entry;
    const uint seed = identity * 0x9E3779B9u ^ h.frame * 0x85EBCA6Bu;

    // Texels: lanes 0..63 (radiance, hit distance).
    if (lane < GI_TEXEL_COUNT)
    {
        const float3 previous = float3(f16tof32(texelOld.x), f16tof32(texelOld.x >> 16), f16tof32(texelOld.y)) * GI_LOAD_SCALE;
        const float3 sample = asfloat(texelSample.xyz);
        const bool finite = all(sample == sample) && all(abs(sample) < 3.0e38);
        const float a = finite ? alpha : 0.0;  // a non-finite value keeps the texel as it was
        float3 value = lerp(previous, finite ? sample : previous, a) * GI_STORE_SCALE;
#if SPLIT
        value = giSplitBlend(b, splitBase, lane, directTexel, reflectedTexel, directAlpha, bounceAlpha, history == 0) * GI_STORE_SCALE;
#endif
        [unroll] for (uint c = 0; c < 3; ++c) value[c] = giDitherHalf(value[c], giDitherUnit(seed + lane * 4 + c));
        const float dist = lerp(f16tof32(texelOld.y >> 16), f16tof32(texelSample.w & 0xFFFFu), a);
        b.Store2(texelAddress, uint2(giPackHalf2(value.r, value.g), giPackHalf2(value.b, dist)));
        // the emitter texel (fourth samples block, GiTrace): same running mean
        const float3 emitPrevious = giIrrUnpack(emitOld) * GI_LOAD_SCALE;
        const bool emitFinite = all(emitted == emitted) && all(abs(emitted) < 3.0e38);
        const uint re = seed + 4096 + lane * 4;
        const float emitAlpha =
#if SPLIT
            directAlpha;
#else
            alpha;
#endif
        b.Store(emitAddress, giPackRgb9e5(lerp(emitPrevious, emitFinite ? emitted : emitPrevious, emitFinite ? emitAlpha : 0.0) * GI_STORE_SCALE,
                                          float3(giDitherUnit(re), giDitherUnit(re + 1), giDitherUnit(re + 2))));
    }
    // Irradiance map: the blend of lanes 0..80.
    if (lane < GI_IRR_N * GI_IRR_N)
    {
        const float3 previous = giIrrUnpack(irrOld) * GI_LOAD_SCALE;
        const uint r = seed + 1024 + lane * 4;
        float3 value = lerp(previous, e, alpha);
#if SPLIT
        value = giSplitBlend(b, splitBase, GI_TEXEL_COUNT + lane, directE, reflectedE, directAlpha, bounceAlpha, history == 0);
#endif
        b.Store(irrAddress, giPackRgb9e5(value * GI_STORE_SCALE, float3(giDitherUnit(r), giDitherUnit(r + 1), giDitherUnit(r + 2))));
    }
    if (lane != 0) return;  // (gs_sh was written before the barrier above)

    float3 previous[9];
    giLoadSh(b, h, entry, previous);
    float v[27];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        float3 e = lerp(previous[k], gs_sh[k], alpha) * GI_STORE_SCALE;
#if SPLIT
        e = giSplitBlend(b, splitBase, GI_TEXEL_COUNT + GI_IRR_N * GI_IRR_N + k, gs_directSh[k], gs_reflectedSh[k],
                         directAlpha, bounceAlpha, history == 0) * GI_STORE_SCALE;
#endif
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
    if (restart) b.InterlockedAdd(GI_P1_STAT_RESTARTS, 1u);
    b.Store(address + GI_SH_RESTART, history == 0 || restart ? 1u : min(restartCount + 1, 65535u));
    b.Store(address + GI_SH_UPDATES, b.Load(address + GI_SH_UPDATES) + 1);
    b.Store(address + GI_SH_LAST_UPDATE, h.frame);
#if SPLIT
    b.Store(splitBase, min(splitUpdates + 1, 4095u));
#endif
    const uint jacobiByte = min((jacobiLength + 15) / 16, 255u) << 24;
    const uint historyWord = prior ? (min(jacobiLength, 0xFFFu) | (min((uint)round(kappa) + 1, max(cap, 2u) - 1u) << 12) | jacobiByte)
                             : restart ? ((history & 0xFFFu) | (1u << 12) | jacobiByte) : giHistoryNext(h, history, young, jacobiLength, cap);
    b.Store3(address + GI_SH_HISTORY, uint3(historyWord, h.epoch,
                                            f32tof16(nearestHalf(fast * GI_STORE_SCALE)) | (f32tof16(nearestHalf(spreadMean * GI_STORE_SCALE)) << 16)));
}
