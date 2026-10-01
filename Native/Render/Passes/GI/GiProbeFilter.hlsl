// unx-kernel: cs_6_6 main
// r.gi.probe.wide, r.gi.probe.filter<k>: the wide layer of L_gi (redesign V2 1.2, P2; gi.screen_wide_filter). Each cache entry is its own
// estimate, and where an update's rays spread widely (a few of the 64 rays reach a small bright region: lamps, lit
// patches) the entries differ by tens of per cent even after hundreds of updates - blotches of a cell's size that the
// per-pixel filter (r.gi.screen.filter: at most ~2 cells, 12 taps) cannot average and that a history cannot either (they
// stay where they are) [observed: bath lounge and train lounge, GI layer at frame 299, 2026-10-01]. Indirect irradiance
// is smooth over many cells except at geometry, so this pass averages it over many cells at once, in the screen probes
// (one per 8 x 8 px: 1/64 of the pixels): an edge-stopping a-trous filter of the probes' irradiance SH (the L2 SH of the
// cache at the probe's surface point, GiProbeGather), 5 x 5 taps of the B3 spline (1/16, 1/4, 3/8, 1/4, 1/16) per pass,
// tap spacing step, 2 step, 4 step probes over three passes (reach +-14 step probes). A tap's weight:
//   plane:     its surface point's distance from the probe's tangent plane, 0 at 2 % of the probe's distance to the eye;
//   normal:    (n . n_tap)^8 (a wall's SH is not a floor's);
//   luminance: exp(-d / (4 sigma)), d = the relative difference of the SH's constant terms (luminance) and sigma = the
//              probe's relative standard deviation: in the first filter pass the lookup's sigma at its surface point
//              (GiCache.hlsli: measured spread / sqrt(samples)), then what each pass leaves of it, sigma x sqrt(sum
//              w^2) / sum w (the variance of the weighted mean it stored, as SVGF propagates its variance). Where the
//              estimate is noisy every tap on the surface counts; as the passes widen, only taps within the value's
//              remaining error do, so a real gradient (larger than 4 sigma of what is left) is kept: the later passes
//              average the cells' errors without flattening the light's own variation over several cells;
//   surface:   probes without a surface, and probes whose point has no cache data yet (SH 0), are left out; a probe
//              without data takes its neighbours' mean (no luminance stop: it has no value of its own).
// The result is each probe's SH in GiProbeWide.hlsli's layout; r.gi.screen.filter evaluates it at the pixel's normal
// through M's probe footprint and takes it where the per-pixel value's sigma is above what the eye accepts.
// The first pass (r.gi.probe.wide, P[1].w = 0) prepares the records: the probe's SH scaled per channel so that its
// irradiance at the probe's normal equals the per-pixel lookup's there (giCacheIrradianceScreen: the 9 x 9 maps with the
// partner corners and the anchor visibility - the value the narrow estimate has; the probes' own SH read differed from
// it by 14 % median, up to x 2 on the lounge's floor, at 128 px tiles [measured 2026-10-01], which would make the wide
// share a change of level). The SH keeps the shape (the response to the pixel's normal), the lookup gives the level; the
// factor is kept within [1/8, 8], and a probe whose SH is 0 where the lookup has data takes the lookup's value as a
// constant term. The same lookup gives the probe's sigma. The filter passes (P[1].w = 1) then read these records only.
// Concept references (no code): Dammertz et al. 2010, edge-avoiding a-trous wavelet filtering; Schied et al. 2017 (SVGF),
// variance-guided luminance stopping; UE5 Lumen's screen probe spatial filter (LumenScreenProbeFiltering.usf).
// Cost: probes x 25 taps x (1 + 4) texel loads per filter pass, 1/64 of the pixels; the first pass one cache lookup per probe.
// P[0] = { probes SRV (view.screenProbes), source SRV (the previous pass' output; UNX_NONE in the first pass), output UAV,
// GI cache SRV (the first pass) }, P[1] = { probesX, probesY, tap spacing (probes), 0 = prepare / 1 = filter }.
// Frame constants b1 = the main view's.
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiProbeWide.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"

float giWideLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

void giWideStore(RWTexture2D<uint4> output, uint2 probe, int2 count, float3 sh[9], uint occlusionWord, float sigma)
{
    uint w[14];
    [unroll] for (uint j = 0; j < 14; ++j) w[j] = 0;
    [unroll] for (uint k = 0; k < 27; ++k)
    {
        const float3 c = sh[k / 3] * GI_STORE_SCALE;
        const float v = (k % 3) == 0 ? c.x : ((k % 3) == 1 ? c.y : c.z);
        w[k >> 1] |= f32tof16(v) << ((k & 1) * 16);
    }
    w[13] |= occlusionWord & 0xFFFF0000u;
    output[uint2(probe.x, probe.y)] = uint4(w[0], w[1], w[2], w[3]);
    output[uint2(probe.x + count.x, probe.y)] = uint4(w[4], w[5], w[6], w[7]);
    output[uint2(probe.x + 2 * count.x, probe.y)] = uint4(w[8], w[9], w[10], w[11]);
    output[uint2(probe.x + 3 * count.x, probe.y)] = uint4(w[12], w[13], asuint(sigma), 0);
}

template <typename Src>
void giWideFilter(Src src, Texture2D<uint4> probes, RWTexture2D<uint4> output, uint2 id, int2 count, float3 p, float3 n, float sigma)
{
    const GiProbeRecord centre = giLoadProbeRecord(src, id, count);
    const uint occlusionWord = giProbePlane(src, id, 4, count).y;
    const float planeTolerance = 0.02 * max(distance(p, g_cameraPosition), 1e-6);
    const float l0 = giWideLuminance(centre.sh[0]);
    const bool has = l0 > 0;
    const float stop = has ? 1.0 / (4.0 * max(sigma, 1e-3)) : 0.0;
    const int step = (int)P[1].z;
    float3 sum[9];
    float weight = has ? 0.375 * 0.375 : 0.0;
    float weight2 = weight * weight;
    [unroll] for (uint k = 0; k < 9; ++k) sum[k] = centre.sh[k] * weight;
    [loop] for (int dy = -2; dy <= 2; ++dy)
    {
        [loop] for (int dx = -2; dx <= 2; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            const int2 q = int2(id) + int2(dx, dy) * step;
            if (any(q < 0) || any(q >= count)) continue;
            float3 qp, qn;
            if (!giLoadProbeSurface(probes, uint2(q), count, qp, qn)) continue;
            const float plane = saturate(1 - abs(dot(n, qp - p)) / planeTolerance);
            float agree = saturate(dot(n, qn));
            agree *= agree;  // ^2
            agree *= agree;  // ^4
            agree *= agree;  // ^8
            float w = plane * plane * agree;
            if (w <= 0) continue;
            const GiProbeRecord r = giLoadProbeRecord(src, uint2(q), count);
            const float lq = giWideLuminance(r.sh[0]);
            if (!(lq > 0)) continue;
            const float d = abs(lq - l0) / max(max(lq, l0), 1e-9);
            const float kx = dx == 0 ? 0.375 : (abs(dx) == 1 ? 0.25 : 0.0625), ky = dy == 0 ? 0.375 : (abs(dy) == 1 ? 0.25 : 0.0625);
            w *= kx * ky * exp(-d * stop);
            [unroll] for (uint c = 0; c < 9; ++c) sum[c] += w * r.sh[c];
            weight += w;
            weight2 += w * w;
        }
    }
    [unroll] for (uint m = 0; m < 9; ++m) sum[m] = weight > 0 ? sum[m] / weight : 0.0;
    giWideStore(output, id, count, sum, occlusionWord, weight > 0 ? sigma * sqrt(weight2) / weight : sigma);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 count = int2(P[1].xy);
    if (any(int2(id) >= count)) return;
    Texture2D<uint4> probes = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint4> output = ResourceDescriptorHeap[P[0].z];
    float3 p, n;
    if (!giLoadProbeSurface(probes, id, count, p, n))
    {
        [unroll] for (uint k = 0; k < 4; ++k) output[uint2(id.x + k * count.x, id.y)] = uint4(0, 0, 0, 0);
        return;
    }
    if (P[1].w == 0)
    {
        GiProbeRecord r = giLoadProbeRecord(probes, id, count);
        ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].w];
        float weight, sigma;
        const float3 e = giCacheIrradianceScreen(cache, giHeader(cache), p, n, weight, sigma);
        if (weight > 0)
        {
            const float3 own = giEvalShIrradiance(r.sh, n);
            if (any(own > 0))
            {
                const float3 scale = float3(own.x > 0 ? clamp(e.x / own.x, 0.125, 8.0) : 1.0, own.y > 0 ? clamp(e.y / own.y, 0.125, 8.0) : 1.0,
                                            own.z > 0 ? clamp(e.z / own.z, 0.125, 8.0) : 1.0);
                [unroll] for (uint k = 0; k < 9; ++k) r.sh[k] *= scale;
            }
            else
            {
                r.sh[0] = e / 0.282095;
                [unroll] for (uint k = 1; k < 9; ++k) r.sh[k] = 0;
            }
        }
        giWideStore(output, id, count, r.sh, giProbePlane(probes, id, 4, count).y, weight > 0 ? sigma : 1.0);
    }
    else
    {
        GiProbeWide src;
        src.srv = P[0].y;
        giWideFilter(src, probes, output, id, count, p, n, asfloat(giProbePlane(src, id, 4, count).z));
    }
}
