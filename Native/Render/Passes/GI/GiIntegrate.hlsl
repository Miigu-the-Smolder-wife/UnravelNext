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
// Redesign V2.2 11.2 (P1'-b), gi.bounce_split (GI_P1_FLAGS bit 4; SPLIT=0): unbiased convergence of the multi-bounce
//   iteration E = S + T E. The map, SH and texels are the long mean M of the whole samples (weight max(1 / (n + 1), 1 / cap):
//   the entry's update count and the steady window, never its neighbours' age - no Jacobi phase, no young weighting), and
//   the stored value is M + (B - Bm), B the current bounce part (the rays' cache-fed radiance, GiTrace's fifth block;
//   weight max(1 / (n + 1), 1 / window), window 1 = replacement) and Bm its long mean (M's weights), both L1 radiance SH
//   (GiInternal giBounceL1Load). The running mean of a weakly contracting iteration converges as n^-(1 - rho); here the
//   bounce part contracts by rho per update as Jacobi does while the non-bounce part (unbiased from its first sample)
//   keeps its long mean. Only the correction B - Bm is truncated to L1 (small once converged); M keeps every direction.
// Window rule (gi.history_window_rule; redesign V2.2 11.2-3, weights = functions of the update count): "lighting" (P[0].w
//   1 or 2) takes the running mean's window from the scene, not from the samples - history_updates_max while the sun or sky
//   changed within gi.lighting_recent_frames (2), history_updates_max_static otherwise (1); local lights and geometry restart
//   their entries (GiInvalidate). "samples" (0, the previous rule) chose it from the entry's own fast mean and spread: a
//   heavy-tailed sample (a lamp's hotspot) moved the fast mean, cut the window and changed the weight of the samples around
//   it - GiAnalytic 8's single bounce beside a lamp came out +7 to +13 % (fixed window: within 0.33 %) [measured, 1920 frames].
// gi.path_guiding (D-12, GiGuide.hlsl; P[1].x = the guide SRV, 0xFFFFFFFF = off): the rays' texels follow the slot's
//   mixture CDF: each ray's solid-angle weight is divided by 64 x its texel's probability (1 / 64 when uniform), and a
//   texel blends the mean of the rays that landed in it (none: kept as it was).
// One group per update slot (selected entries, then background). P[0] = { cache UAV, updates per frame, samples SRV,
// window rule }, P[1] = { guide SRV }
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
groupshared uint gs_rayTexel[GI_TEXEL_COUNT];   // gi.path_guiding: each texel ray's texel
groupshared float4 gs_rayTexelValue[GI_TEXEL_COUNT];  // its texel sample (radiance, hit distance)
// gi.miss_closure (GI_P1_FLAGS bit 5): a ray whose hit found no bounce data (GiTrace: no cell with an update - after a
// cut, where the view opens - or, with gi.bounce_visibility, none whose anchor sees the hit) read irradiance 0 there: the entry
// started at its first bounce alone, 50-80 % of its level in the bath and train scenes, and rose over ~16 frames as the
// hit cells got their updates [measured 2026-10-01]. The closure takes the entry's own irradiance E for the missing
// irradiance at those hits (the surfaces an entry sees are lit by the same room as the entry; the ambient term of
// progressive radiosity, per entry): E = E_measured + sum over the missing rays of (albedo / pi) w cos x E, so
// E = E_measured / (1 - R), R = sum (albedo / pi) w cos <= 0.9 per channel, and each missing ray's radiance gets
// (albedo / pi) x E before the sums below (map, SH, texels, statistics: all as if the ray had read E). No missing ray:
// nothing changes. Hit cells with data are read as before, so the closure fades as the cache fills.
groupshared float3 gs_miss[GI_TEXEL_COUNT];  // per texel ray: albedo / pi of a hit without bounce data, else 0
groupshared float3 gs_closure;               // E
groupshared float3 gs_rayEmitted[GI_TEXEL_COUNT];     // its emitter texel sample
#if !SPLIT
groupshared float3 gs_bounceRay[GI_TEXEL_COUNT];  // gi.bounce_split: each texel ray's cache-fed radiance
groupshared float3 gs_bounceSample[4];            // its L1 radiance SH (world frame)
#endif

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
    const bool bsplit = (p1 & 16u) != 0;
    const bool closure = (p1 & 32u) != 0;
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
        float probability = 1.0 / GI_TEXEL_COUNT;
        const uint rayTexel = min((uint)(uv.x * GI_TEXELS), GI_TEXELS - 1) + GI_TEXELS * min((uint)(uv.y * GI_TEXELS), GI_TEXELS - 1);
        if (P[1].x != 0xFFFFFFFFu)
        {
            StructuredBuffer<float> guide = ResourceDescriptorHeap[P[1].x];
            const uint base = slot * GI_TEXEL_COUNT;
            probability = max(guide[base + rayTexel] - (rayTexel > 0 ? guide[base + rayTexel - 1] : 0.0), 1e-9);
        }
        gs_rayTexel[lane] = rayTexel;
        gs_sample[lane] = finite ? float4(value, 2 / (len * len * len) / (float)GI_TEXEL_COUNT / (probability * GI_TEXEL_COUNT)) : float4(0, 0, 0, 0);
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
#if !SPLIT
        if (bsplit)
        {
            const float3 br = asfloat(samples[4 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane].xyz);
            gs_bounceRay[lane] = finite && all(br == br) && all(abs(br) < 3.0e38) ? br : float3(0, 0, 0);
        }
#endif
        const uint w = texelSample.w;
        gs_bounce[lane] = float2((w >> 17) & 1u, (w >> 16) & 1u);
        const float lum = dot(asfloat(texelSample.xyz), float3(0.2126, 0.7152, 0.0722));
        gs_bounceIrradiance[lane] = finite && lum < 3.0e38 ? ((w >> 18) & 4095u) / 4095.0 * lum * gs_sample[lane].w * gs_local[lane].z : 0.0;
        texelOld = b.Load2(texelAddress);
        const uint4 emitterSample = samples[3 * P[0].y * GI_TEXEL_COUNT + slot * GI_TEXEL_COUNT + lane];
        emitted = asfloat(emitterSample.xyz);
        gs_miss[lane] = closure && finite && (emitterSample.w >> 31) != 0
                            ? float3(emitterSample.w & 1023u, (emitterSample.w >> 10) & 1023u, (emitterSample.w >> 20) & 1023u) / (1023.0 * GI_PI) : float3(0, 0, 0);
        emitOld = b.Load(emitAddress);
        gs_rayTexelValue[lane] = float4(asfloat(texelSample.xyz), f16tof32(texelSample.w & 0xFFFFu));
        gs_rayEmitted[lane] = emitted;
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
    if (closure)  // (uniform over the group)
    {
        if (lane < WaveGetLaneCount())
        {
            float3 measured = 0, transfer = 0;
            [loop] for (uint k = 0; k < 2 * GI_TEXEL_COUNT; ++k) measured += gs_sample[k].xyz * (gs_sample[k].w * max(gs_local[k].z, 0.0));
            [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) transfer += gs_miss[k] * (gs_sample[k].w * max(gs_local[k].z, 0.0));
            if (WaveIsFirstLane()) gs_closure = measured / (1 - min(transfer, 0.9));
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane < GI_TEXEL_COUNT)
        {
            const float3 add = gs_miss[lane] * gs_closure;
            gs_sample[lane].xyz += add;
            gs_rayTexelValue[lane].xyz += add;
            texelSample.xyz = asuint(asfloat(texelSample.xyz) + add);
            gs_bounceIrradiance[lane] += dot(add, float3(0.2126, 0.7152, 0.0722)) * gs_sample[lane].w * gs_local[lane].z;
#if !SPLIT
            if (bsplit) gs_bounceRay[lane] += add;
#endif
        }
        GroupMemoryBarrierWithGroupSync();
    }
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
#if !SPLIT
        if (bsplit && shLane < 4)
        {
            float3 cb = 0;
            [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k) cb += gs_bounceRay[k] * gs_shWeight[k * 9 + shLane];
            gs_bounceSample[shLane] = cb;
        }
#endif
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
    const bool steady = P[0].w != 0 ? P[0].w == 1 : history != 0 && abs(fastOld - held) <= 3 * spreadOld / sqrt(31.0) + 0.01 * held;
    const float spread = sqrt(max(totalE2 - totalE * totalE / (2.0 * GI_TEXEL_COUNT), 0.0));
    const bool windowReset = history == 0 || ((p1 & 4u) != 0 && history != 0 && restartCount >= 4 && abs(totalE - held) > 5.0 * spreadOld + 0.05 * held);
    const float fast = windowReset ? totalE : lerp(fastOld, totalE, 1.0 / 16.0);
    const float spreadMean = windowReset ? spread : lerp(spreadOld, spread, 1.0 / 16.0);
    const uint cap = steady ? h.historyStatic : h.historyMax;
    const float alphaHistory = giHistoryAlpha(h, history, young, jacobiLength, cap);
    const bool restart = (p1 & 4u) != 0 && history != 0 && restartCount >= 4 && abs(totalE - held) > 5.0 * spreadOld + 0.05 * held;
    const float relSpread = totalE > 0 ? spread / totalE : 1.0;
    const float kappa = clamp(relSpread * relSpread / max(asfloat(b.Load(GI_P1_DELTA2)), 1e-6), 2.0, 64.0);
    float alpha = prior ? 1.0 / (1.0 + kappa) : (restart ? 0.5 : alphaHistory);
    // gi.bounce_split: the weight of M and Bm (update count and window alone) and of the current B; the entry's L1 pair
    // before this update (0 after a reset; a seeded entry has its parent's, GiPrior) and after it (rounded as stored, the
    // same in every lane), and the corrections B - Bm of both.
    float3 dOld[4], dNew[4], bCurNew[4], bMeanNew[4];
    [unroll] for (uint c0 = 0; c0 < 4; ++c0) dOld[c0] = dNew[c0] = bCurNew[c0] = bMeanNew[c0] = 0;
#if !SPLIT
    if (bsplit)
    {
        const float n = prior ? kappa : (history == 0 ? 0.0 : (float)giHistorySamples(history));
        alpha = prior ? 1.0 / (1.0 + kappa) : max(1.0 / (n + 1), 1.0 / (float)cap);
        const float alphaB = max(1.0 / (n + 1), 1.0 / (float)max(b.Load(GI_BSPLIT_WINDOW), 1u));
        float3 bCur[4], bMean[4];
        giBounceL1Load(b, entry, bCur, bMean);
        const uint dseed = (((h.flags & 1u) != 0 ? giDetKey(b, h, entry) : entry) * 0x9E3779B9u ^ h.frame * 0x85EBCA6Bu) + 8192u;
        [unroll] for (uint c = 0; c < 4; ++c)
        {
            if (history == 0 && !prior) bCur[c] = bMean[c] = 0;
            dOld[c] = bCur[c] - bMean[c];
            const float3 cn = lerp(bCur[c], gs_bounceSample[c], alphaB) * GI_STORE_SCALE;
            const float3 mn = lerp(bMean[c], gs_bounceSample[c], alpha) * GI_STORE_SCALE;
            [unroll] for (uint ch = 0; ch < 3; ++ch)
            {
                bCurNew[c][ch] = giDitherHalf(cn[ch], giDitherUnit(dseed + c * 8 + ch)) * GI_LOAD_SCALE;
                bMeanNew[c][ch] = giDitherHalf(mn[ch], giDitherUnit(dseed + c * 8 + 4 + ch)) * GI_LOAD_SCALE;
            }
            dNew[c] = bCurNew[c] - bMeanNew[c];
        }
    }
#endif
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
        float3 sample = asfloat(texelSample.xyz);
        bool finite = all(sample == sample) && all(abs(sample) < 3.0e38);
        float sampleDistance = f16tof32(texelSample.w & 0xFFFFu);
        if (P[1].x != 0xFFFFFFFFu)
        {
            // gi.path_guiding: the mean of the finite rays that landed in this texel (lane = texel)
            float4 sum = 0;
            float3 emitSum = 0;
            uint count = 0;
            [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k)
            {
                const float4 v = gs_rayTexelValue[k];
                if (gs_rayTexel[k] != lane || !(all(v.xyz == v.xyz) && all(abs(v.xyz) < 3.0e38))) continue;
                sum += v;
                emitSum += gs_rayEmitted[k];
                ++count;
            }
            finite = count > 0;
            sample = finite ? sum.xyz / count : previous;
            sampleDistance = finite ? sum.w / count : 0;
            emitted = finite ? emitSum / count : 0;
        }
        const float a = finite ? alpha : 0.0;  // a non-finite value (or, guided, no ray in the texel) keeps the texel as it was
        float3 value = lerp(previous, finite ? sample : previous, a) * GI_STORE_SCALE;
        if (bsplit)
        {
            // M = stored - (B - Bm) toward the texel's centre direction (radiance: the SH basis alone)
            const float3 l = giHemiOctDecode((float2(lane % GI_TEXELS, lane / GI_TEXELS) + 0.5) / GI_TEXELS);
            float y[9];
            giShBasis(t * l.x + bt * l.y + na * l.z, y);
            float3 before = 0, after = 0;
            [unroll] for (uint c = 0; c < 4; ++c)
            {
                before += dOld[c] * y[c];
                after += dNew[c] * y[c];
            }
            const float3 m = previous - before;
            value = (lerp(m, finite ? sample : m, a) + after) * GI_STORE_SCALE;
        }
#if SPLIT
        value = giSplitBlend(b, splitBase, lane, directTexel, reflectedTexel, directAlpha, bounceAlpha, history == 0) * GI_STORE_SCALE;
#endif
        [unroll] for (uint c = 0; c < 3; ++c) value[c] = giDitherHalf(value[c], giDitherUnit(seed + lane * 4 + c));
        const float dist = lerp(f16tof32(texelOld.y >> 16), sampleDistance, a);
        b.Store2(texelAddress, uint2(giPackHalf2(value.r, value.g), giPackHalf2(value.b, dist)));
        // the emitter texel (fourth samples block, GiTrace): same running mean
        const float3 emitPrevious = giIrrUnpack(emitOld) * GI_LOAD_SCALE;
        const bool emitFinite = all(emitted == emitted) && all(abs(emitted) < 3.0e38) && (P[1].x == 0xFFFFFFFFu || finite);
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
        if (bsplit)
        {
            const float3 nj = giHemiOctDecode((float2(lane % GI_IRR_N, lane / GI_IRR_N) + 0.5) / (float)GI_IRR_N);
            float y[9];
            giShBasis(t * nj.x + bt * nj.y + na * nj.z, y);
            float3 before = GI_PI * dOld[0] * y[0], after = GI_PI * dNew[0] * y[0];  // irradiance: A_0 = pi, A_1 = 2 pi / 3
            [unroll] for (uint c = 1; c < 4; ++c)
            {
                before += (2 * GI_PI / 3) * dOld[c] * y[c];
                after += (2 * GI_PI / 3) * dNew[c] * y[c];
            }
            value = lerp(previous - before, e, alpha) + after;
        }
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
        if (bsplit && k < 4)
        {
            const float A = k == 0 ? GI_PI : 2 * GI_PI / 3;
            e = (lerp(previous[k] - A * dOld[k], gs_sh[k], alpha) + A * dNew[k]) * GI_STORE_SCALE;
        }
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
    if (history == 0 && !prior) giAccClear(b, entry);  // gi.hit_accumulator: a restart (the means of an older state)
    if (restart) b.InterlockedAdd(GI_P1_STAT_RESTARTS, 1u);
    b.Store(address + GI_SH_RESTART, history == 0 || restart ? 1u : min(restartCount + 1, 65535u));
    b.Store(address + GI_SH_UPDATES, b.Load(address + GI_SH_UPDATES) + 1);
    b.Store(address + GI_SH_LAST_UPDATE, h.frame);
#if SPLIT
    b.Store(splitBase, min(splitUpdates + 1, 4095u));
#endif
#if !SPLIT
    if (bsplit)
    {
        const uint a = b.Load(GI_BSPLIT_OFFSET) + entry * 48;
        float v[24];
        [unroll] for (uint c = 0; c < 4; ++c)
            [unroll] for (uint ch = 0; ch < 3; ++ch)
            {
                v[3 * c + ch] = bCurNew[c][ch] * GI_STORE_SCALE;
                v[12 + 3 * c + ch] = bMeanNew[c][ch] * GI_STORE_SCALE;
            }
        uint w[12];
        [unroll] for (uint i = 0; i < 12; ++i) w[i] = giPackHalf2(v[2 * i], v[2 * i + 1]);
        b.Store4(a, uint4(w[0], w[1], w[2], w[3]));
        b.Store4(a + 16, uint4(w[4], w[5], w[6], w[7]));
        b.Store4(a + 32, uint4(w[8], w[9], w[10], w[11]));
    }
#endif
    {
        // gi.anchor_resample (GiInternal giAnchorOffer): the next update's anchor, a uniform choice among this one's lookups
        const uint rb = b.Load(GI_RESAMPLE_OFFSET);
        if (rb != 0)
        {
            const uint2 offer = b.Load2(rb + entry * 8);
            if ((offer.x | offer.y) != 0)
            {
                const uint2 key = b.Load2(h.offMeta + entry * 16);
                const uint64_t o = ((uint64_t)offer.y << 32) | offer.x;
                b.Store3(h.offAnchor + entry * 16, asuint(giAnchorOfferPosition(h, ((uint64_t)key.y << 32) | key.x, o & ((1ull << 42) - 1))));
                b.Store2(rb + entry * 8, uint2(0, 0));
            }
        }
    }
    // gi.anchor_centroid (GiInternal giCentroidOffer): the period's lookups into the centroid; a move beyond s / 8 places
    // the anchor there (or at the nearest lookup when the centroid left the surface) and halves the running-mean count.
    bool centroidMoved = false;
    {
        const uint cb = b.Load(GI_CENTROID_OFFSET);
        if (cb != 0)
        {
            const uint a = cb + entry * 48;
            const uint4 sums = b.Load4(a);
            if (sums.w > 0)
            {
                const uint2 keyWords = b.Load2(h.offMeta + entry * 16);
                const uint64_t key = ((uint64_t)keyWords.y << 32) | keyWords.x;
                const float s = giCellSize(h, (uint)(key & 31u));
                const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;
                const float3 origin = float3(cell) * s - 0.5 * s;
                const float3 mean = origin + float3(sums.xyz) / (float)sums.w / 16383.0 * (2 * s);
                const float4 old = asfloat(b.Load4(a + 32));
                const float alpha = 1.0 - pow(63.0 / 64.0, (float)sums.w);
                const float3 meanPoint = old.w > 0 ? old.xyz + (mean - old.xyz) * alpha : mean;
                const float3 anchorNow = asfloat(b.Load3(h.offAnchor + entry * 16));
                if (distance(meanPoint, anchorNow) > s / 8)
                {
                    float3 target = meanPoint;
                    const uint2 nearestWords = b.Load2(a + 16);
                    if (nearestWords.y != 0xFFFFFFFFu)
                    {
                        const uint64_t nearest = ((uint64_t)nearestWords.y << 32) | nearestWords.x;
                        const float3 p = giAnchorOfferPosition(h, key, nearest & ((1ull << 42) - 1));
                        if (distance(meanPoint, p) > s / 8) target = p;  // the centroid lies off the looked-up surface
                    }
                    b.Store3(h.offAnchor + entry * 16, asuint(target));
                    centroidMoved = true;
                }
                b.Store4(a + 32, asuint(float4(meanPoint, 1)));
                b.Store4(a, uint4(0, 0, 0, 0));
                b.Store2(a + 16, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
            }
        }
    }
    const uint jacobiByte = min((jacobiLength + 15) / 16, 255u) << 24;
    uint historyWord = prior ? (min(jacobiLength, 0xFFFu) | (min((uint)round(kappa) + 1, max(cap, 2u) - 1u) << 12) | jacobiByte)
                             : restart ? ((history & 0xFFFu) | (1u << 12) | jacobiByte) : giHistoryNext(h, history, bsplit ? 0.0 : young, jacobiLength, cap);
    if (centroidMoved)
    {
        // the running mean's count (bits 12..23, GiInternal giHistory) halved: the moved anchor's irradiance weighs in
        const uint count = (historyWord >> 12) & 0xFFFu;
        historyWord = (historyWord & ~(0xFFFu << 12)) | (max(count / 2, 1u) << 12);
    }
    b.Store3(address + GI_SH_HISTORY, uint3(historyWord, h.epoch,
                                            f32tof16(nearestHalf(fast * GI_STORE_SCALE)) | (f32tof16(nearestHalf(spreadMean * GI_STORE_SCALE)) << 16)));
}
