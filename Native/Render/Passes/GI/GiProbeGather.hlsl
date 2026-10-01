// unx-kernel: cs_6_6 main
// Screen probe fill (no rays): trilinear cache SH at the probe's surface point, the choice of the cache entry with the
// largest trilinear weight as the probe's radiance map source (copied by GiProbeMaps once per entry: the lowest probe
// index reading it owns the copy), and near occlusion there: 16 fixed
// cosine-distributed hemisphere points within r = min(gi.near_occlusion_radius_m, cache cell edge) — occlusion below
// the cache's own resolution only, so the cache (whose rays already see larger occluders) is not darkened twice.
// A point is occluded when the depth buffer shows a surface in front of it closer than r along the view ray.
// The occlusion is integrated over time per probe (gi.screen_occlusion_history_frames): the 16 points turn by a golden-
// ratio step every frame, and the probe's surface point is found in the previous frame's probes by its exact motion
// (GiScreenHistory.hlsli), where the probe there on the same plane with the same normal carries its running mean. One
// frame's 16 points quantise the occlusion to 1/16 per probe, and neighbouring probes' different point sets drew an 8 x 8
// px pattern wherever something occludes (corners, contacts), fixed to the screen probe grid (a mosaic sliding over the
// surfaces in motion). Occlusion depends on geometry only, so the window costs no lighting lag; a moving occluder is
// followed by a clamp of the history to the frame's own estimate +- 3 sigma of its 16-point binomial spread (UE5 Lumen's
// screen probe gather integrates its probes over time the same way: concept only, Engine/Shaders/Private/Lumen/
// LumenScreenProbeGather.usf, temporal reprojection of the probes with depth and normal rejection).
// Spatial reconstruction (redesign V2 1.2, layer L_occ; gi.screen_occlusion_spatial, P[4].x bit 1): where the history is
// rejected (a cut, a disocclusion, the first frames) the 16-point estimate alone showed that mosaic again. Each probe now
// takes this frame's estimates of the 3 x 3 probes around it (their own points, recomputed here: 144 points) with a
// 1-2-1 tent, each neighbour weighted by
//   plane, normal: it lies on the probe's tangent plane (2 % of the depth) and faces its way (power 4) - the history's rule;
//   value:         exp(-(difference / 3 sigma)^2), sigma the probe's 16-point binomial spread (floor 1/16): neighbours
//                  that differ by noise are averaged, a real occlusion gradient (a crease, a contact: differences of
//                  several sigma between probes 8 px apart) keeps the probe's own value.
// The history then integrates the reconstructed value and is clamped to it within 3 sigma of the reconstruction's own
// spread (sigma x sqrt(sum w^2) / sum w): a frame without history is already the 144-point value.
// P[0] = { cache UAV (raw), depth SRV, gbuffer SRV, probes UAV }, P[1] = { probesX, probesY, width, height },
// P[2] = { spacing, near occlusion radius (float bits), map owner list UAV, map dispatch args UAV } (reset here for
// GiProbeMapOwners), P[3] = { vis id SRV (UNX_NONE: no history), visible clusters SRV, previous probe history UAV, probe
// history UAV }, P[4] = { flags (bit 0: reset the history, bit 1: spatial reconstruction), history frames, previous probesX,
// previous probesY };
// frame constants b1 = main view.
// Probe history (persistent, RGBA32_UINT (2 probesX) x probesY): texel (2i, j) = { world position (f32 x 3), packed
// normal (0 = no surface) }, texel (2i + 1, j) = { occlusion mean (f32), frames in it, 0, 0 }.
#include "GBuffer.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

// The occlusion history of the probe's surface point from the previous frame's probes (bilinear over the four around its
// previous pixel, those on its plane and facing its way); n = the frames in it (0: none).
float giProbeOcclusionHistory(uint2 pixel, float3 p, float3 normal, uint spacing, out uint n)
{
    n = 0;
    if (P[3].x == UNX_NONE || (P[4].x & 1u) != 0) return 0;
    float3 prevP, prevN;
    uint instance;
    giPreviousSurface(P[3].x, P[3].y, pixel, p, normal, prevP, prevN, instance);
    float2 prevPixel;
    float prevDepth;
    if (!giPreviousPixel(prevP, float2(P[1].zw), prevPixel, prevDepth)) return 0;
    const int2 prevCount = int2(P[4].zw);
    RWTexture2D<uint4> history = ResourceDescriptorHeap[P[3].z];
    const float2 f = (prevPixel - 0.5) / spacing;  // probe (i, j) sits at pixel (spacing i, spacing j) (centre + 0.5)
    const int2 i0 = int2(floor(f));
    const float2 fr = f - floor(f);
    float sum = 0, weight = 0;
    uint frames = 0xFFFFFFFFu;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = i0 + o;
        if (any(q < 0) || any(q >= prevCount)) continue;
        const uint4 a = history[uint2(2 * q.x, q.y)];
        if (a.w == 0) continue;
        const float3 qp = asfloat(a.xyz);
        const float plane = saturate(1 - abs(dot(prevN, qp - prevP)) / max(prevDepth, 1e-6) / 0.02);
        const float agree = saturate(dot(prevN, giUnpackAnchorNormal(a.w)));
        const float agree2 = agree * agree;
        const float w = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) * (plane * plane) * (agree2 * agree2);
        if (w <= 0) continue;
        const uint4 b = history[uint2(2 * q.x + 1, q.y)];
        sum += w * asfloat(b.x);
        weight += w;
        frames = min(frames, b.y);
    }
    if (weight < 1e-4 || frames == 0xFFFFFFFFu) return 0;
    n = frames;
    return sum / weight;
}

float nearOcclusion(Texture2D<float> depth, float3 p, float3 n, float radius, uint rotation, uint2 size)
{
    float3 t, bt;
    giBasis(n, t, bt);
    const float phase = (rotation & 1023u) * (6.28318530718 / 1024.0);
    float occluded = 0, total = 0;
    [unroll] for (uint i = 0; i < 16; ++i)
    {
        // Cosine-weighted hemisphere (Vogel spiral on the disk, lifted), radius fraction sqrt-stratified.
        const float u = (i + 0.5) / 16.0;
        const float phi = i * 2.39996323 + phase;
        const float sr = sqrt(u);
        const float3 local = float3(sr * cos(phi), sr * sin(phi), sqrt(1 - u));
        const float reach = radius * sqrt((i % 4 + 0.5) / 4.0);
        const float3 q = p + (t * local.x + bt * local.y + n * local.z) * reach;
        const float4 clip = mul(g_viewProj, float4(q, 1));
        if (clip.w <= 0) continue;
        const float2 ndc = clip.xy / clip.w;
        const int2 pixel = int2((ndc * float2(0.5, -0.5) + 0.5) * float2(size));
        total += 1;
        if (any(pixel < 0) || any(pixel >= int2(size))) continue;
        const float device = depth.Load(int3(pixel, 0));
        const float sceneDepth = linearDepth(device);
        const float sampleDepth = clip.w;  // view distance along the axis (reversed-Z infinite projection: w = view depth)
        // An occluder is a surface in front of the point that also rises above the probe's tangent plane. The depth
        // comparison alone made a plane occlude itself at grazing views: its depth changes by much more than the
        // point's height across one pixel, so the pixel centre's depth read nearer than a point just above it (dark
        // one-pixel lines on walls at the probe columns, D0 2026-09-26).
        if (sceneDepth < sampleDepth - 1e-3 * sampleDepth && sampleDepth - sceneDepth < radius &&
            dot(worldFromDepth(float2(pixel), device) - p, n) > max(0.05 * reach, 2e-4 * sampleDepth))
            occluded += 1;
    }
    return total > 0 ? 1 - occluded / total : 1;
}

// This frame's 16-point occlusion estimate of probe 'probe' with its surface point and normal (false: no surface there).
bool giProbeOcclusionEstimate(Texture2D<float> depth, Texture2D<uint2> gbuffer, GiHeader h, int2 probe, uint2 count, uint spacing, uint2 size, bool turning,
                              out float3 p, out float3 n, out float raw)
{
    p = n = 0;
    raw = 1;
    if (any(probe < 0) || any(probe >= int2(count))) return false;
    uint2 pixel;
    float d;
    if (!giProbePixel(depth, uint2(probe), spacing, size, pixel, d)) return false;
    p = worldFromDepth(float2(pixel), d);
    n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    const float radius = min(asfloat(P[2].y), giCellSize(h, giLevel(h, p)));
    const uint rotation = (uint)probe.x * 7919u + (uint)probe.y * 104729u + (turning ? h.frame * 633u : 0u);
    raw = nearOcclusion(depth, p + n * (1e-3 * linearDepth(d)), n, radius, rotation, size);
    return true;
}

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    const uint2 count = P[1].xy, size = P[1].zw;
    const uint spacing = P[2].x;
    RWTexture2D<uint4> probes = ResourceDescriptorHeap[P[0].w];
    if (all(probe == 0))
    {
        probes[uint2(0, count.y * 5)] = uint4(spacing, count.x, count.y, 0);
        RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].z];
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[2].w];
        list.Store(0, 0u);
        args.Store4(0, uint4(512, 0, 1, 0));
    }
    if (probe.x >= count.x || probe.y >= count.y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const GiHeader h = giHeader(b);
    uint2 pixel;
    float d;
    float3 c[9];
    const bool keepHistory = P[3].x != UNX_NONE;
    if (!giProbePixel(depth, probe, spacing, size, pixel, d))
    {
        [unroll] for (uint k = 0; k < 9; ++k) c[k] = 0;
        giStoreProbe(probes, probe, count, c, float3(0, 0, 0), float3(0, 0, 1), 1, false);
        probes[uint2(probe.x * 8 + 5, probe.y * 4 + 3)] = uint4(GI_ENTRY_PENDING, 0, 0, 0);  // radiance map source (GiProbeMaps)
        if (keepHistory)
        {
            RWTexture2D<uint4> next = ResourceDescriptorHeap[P[3].w];
            next[uint2(2 * probe.x, probe.y)] = uint4(0, 0, 0, 0);
        }
        return;
    }
    const float3 p = worldFromDepth(float2(pixel), d);
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    uint mapEntry;
    giCacheShAt(b, h, p, n, c, mapEntry);
    probes[uint2(probe.x * 8 + 5, probe.y * 4 + 3)] = uint4(mapEntry, 0, 0, 0);  // radiance map source (GiProbeMaps)
    if (mapEntry != GI_ENTRY_PENDING) b.InterlockedMin(b.Load(GI_H_MAP_OWNER) + mapEntry * 4, probe.y * count.x + probe.x);
    const float radius = min(asfloat(P[2].y), giCellSize(h, giLevel(h, p)));
    // the points turn by 633 / 1024 of a turn per frame (golden ratio): successive frames fill the circle evenly
    const uint rotation = probe.x * 7919u + probe.y * 104729u + (keepHistory ? h.frame * 633u : 0u);
    float raw = nearOcclusion(depth, p + n * (1e-3 * linearDepth(d)), n, radius, rotation, size);
    // one frame's 16 points: binomial spread, floor 1/16 (the estimate's step)
    float sigma = sqrt(max(raw * (1 - raw), 1.0 / 16.0) / 16.0);
    if (P[4].x & 2u)
    {
        const float linearZ = linearDepth(d);
        float sum = 4 * raw, weight = 4, weight2 = 16;
        [loop] for (uint k = 0; k < 9; ++k)
        {
            if (k == 4) continue;
            const int2 o = int2(k % 3, k / 3) - 1;
            float3 pk, nk;
            float rawK;
            if (!giProbeOcclusionEstimate(depth, gbuffer, h, int2(probe) + o, count, spacing, size, keepHistory, pk, nk, rawK)) continue;
            const float plane = saturate(1 - abs(dot(n, pk - p)) / max(linearZ, 1e-6) / 0.02);
            const float agree = saturate(dot(n, nk));
            const float agree2 = agree * agree;
            const float difference = (rawK - raw) / (3 * sigma);
            const float w = (o.x == 0 || o.y == 0 ? 2.0 : 1.0) * (plane * plane) * (agree2 * agree2) * exp(-difference * difference);
            sum += w * rawK;
            weight += w;
            weight2 += w * w;
        }
        raw = sum / weight;
        sigma *= sqrt(weight2) / weight;
    }
    float occlusion = raw;
    uint frames = 0;
    if (keepHistory)
    {
        uint n0;
        const float previous = giProbeOcclusionHistory(pixel, p, n, spacing, n0);
        if (n0 > 0)
        {
            // a moving occluder: the history within 3 sigma of this frame's estimate (16 points, or the reconstruction's)
            const uint m = min(n0, max(P[4].y, 1u) - 1);
            occlusion = lerp(clamp(previous, raw - 3 * sigma, raw + 3 * sigma), raw, 1.0 / (m + 1));
            frames = m + 1;
        }
        else frames = 1;
        RWTexture2D<uint4> next = ResourceDescriptorHeap[P[3].w];
        next[uint2(2 * probe.x, probe.y)] = uint4(asuint(p), max(giPackNormal(n), 1u));
        next[uint2(2 * probe.x + 1, probe.y)] = uint4(asuint(occlusion), frames, 0, 0);
    }
    giStoreProbe(probes, probe, count, c, p, n, occlusion, true);
}
