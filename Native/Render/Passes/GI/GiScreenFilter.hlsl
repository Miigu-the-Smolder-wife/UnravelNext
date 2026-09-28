// unx-kernel: cs_6_6 main
// r.gi.screen.filter: the edge-preserving spatial filter of r.gi.screen's per-pixel cache irradiance (M's front GI
// irradiance, view.giIrradiance). Each cache entry is its own estimate (64 rays per update, a running mean), and the
// pixels between entries interpolate them trilinearly, so what remains of every entry's error is a blotch of its cell's
// size (the cells subtend ~gi.cache_cell_angle_deg: 18-36 px at 4K, 9-18 px at 1080p) that does not move with the
// camera; young cells (new levels while moving, Jacobi phase) are the worst. The filter averages each pixel with the
// surface around it over gi.screen_filter_cells of its cell edge: 12 taps on a golden-angle spiral (area-uniform) with a
// Gaussian falloff (sigma = radius / 2), each weighted by
//   plane:  the tap's distance from the pixel's tangent plane, 0 at 2 % of the view depth (the probe footprint's rule);
//   normal: (n . n_tap)^32 - taps whose normal differs keep their own value: the normal response of the irradiance
//           (normal maps, curved surfaces) stays the pixel's, only the cell-scale error is averaged;
//   data:   taps whose cache lookup found nothing are left out.
// A pixel whose own lookup found nothing takes its neighbours' value (a = 1) when some tap has one; M then does not fall
// back to the screen probes' SH there. The tap pattern is the same every frame (no per-frame rotation: nothing
// integrates it over time, and the field is smooth at the tap spacing).
// Indirect irradiance is smooth where the cells are small against its variation; the filter spans less than one cell,
// the resolution the cache already has, so a real gradient loses at most what one cell of trilinear interpolation does.
// (Concept reference only, no code: the spatial filtering of screen probes in UE5 Lumen, Engine/Shaders/Private/Lumen/
// LumenScreenProbeFiltering.usf, which weights neighbours by plane distance and normal; SVGF-style edge-stopping weights.)
// The main view's output then carries the screen probes' near occlusion (the probe footprint M's gather evaluates, same
// function and inputs, ScreenProbes.hlsli): rgb = E x occlusion x exposure, the pixel's whole front diffuse indirect
// irradiance, so M's shading kernel needs no probe gather for it (only the K path's radiance and Foliage's back side
// gather). Views without probes (planar views): occlusion 1. gi.screen_filter_cells = 0: no spatial filter, the
// occlusion still applied (M relies on it).
// P[0] = { r.gi.screen SRV (RGBA16F: rgb = E x exposure, a = 1 data), depth SRV, gbuffer SRV, output UAV }
// P[1] = { width, height, GI cache SRV (raw), asuint(pixel angle, radians) }, P[2] = { asuint(radius in cell edges), view.screenProbes
// SRV (UNX_NONE: none), 0, 0 }
// Frame constants b1 = the view's.
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"

// The near occlusion of M's probe footprint at the pixel (giProbeGatherFrom -> giFootprintIrradiance's a, without the SH).
float giScreenOcclusion(uint2 pixel, float3 p, float3 n, float linearZ)
{
    if (P[2].y == UNX_NONE) return 1;
    Texture2D<uint4> t = ResourceDescriptorHeap[P[2].y];
    float spacing;
    int2 count;
    const GiProbeFootprint fp = giProbeFootprintAt(t, pixel, p, n, linearZ, spacing, count);
    float occlusion = 0;
    bool any = false;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        if (fp.weight[k] <= 0) continue;
        occlusion += fp.weight[k] * ((giProbePlane(t, uint2(fp.probe[k]), 4, count).y >> 16) / 65535.0);  // giLoadProbeRecord's word 13
        any = true;
    }
    return any ? occlusion : 1;
}

#define GI_FILTER_TAPS 12u
#define GI_FILTER_MAX_PX 48.0

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(pixel >= size)) return;
    Texture2D<float4> raw = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    const float depthValue = depth.Load(int3(pixel, 0));
    float3 p, n;
    if (!giScreenInputs(pixel, depthValue, gbuffer.Load(int3(pixel, 0)), p, n))
    {
        output[pixel] = float4(0, 0, 0, 0);
        return;
    }
    const float4 centre = raw.Load(int3(pixel, 0));
    const float linearZ = linearDepth(depthValue);
    const float occlusion = giScreenOcclusion(pixel, p, n, linearZ);
    ByteAddressBuffer cache = ResourceDescriptorHeap[P[1].z];
    const GiHeader h = giHeader(cache);
    const float cellPx = giCellSize(h, giLevel(h, p)) / max(linearZ * asfloat(P[1].w), 1e-9);
    const float radius = min(asfloat(P[2].x) * cellPx, GI_FILTER_MAX_PX);
    if (radius < 1.5)
    {
        output[pixel] = float4(centre.rgb * occlusion, centre.a);
        return;
    }
    float3 sum = centre.a > 0 ? centre.rgb : 0;
    float weight = centre.a > 0 ? 1 : 0;
    const float planeTolerance = 0.02 * linearZ;
    [loop] for (uint i = 0; i < GI_FILTER_TAPS; ++i)
    {
        const float u = (i + 0.5) / GI_FILTER_TAPS;
        const float r = radius * sqrt(u);
        const float phi = i * 2.39996323;
        const int2 q = int2(pixel) + int2(round(r * float2(cos(phi), sin(phi))));
        if (any(q < 0) || any(q >= int2(size))) continue;
        // The tap's value and depth in one round trip (a tap without data ignores its depth).
        const float4 e = raw.Load(int3(q, 0));
        const float qd = depth.Load(int3(q, 0));
        if (!(e.a > 0)) continue;
        // The tap's position first (giScreenInputs' formula); off the pixel's plane (weight 0) its normal is not needed:
        // the G-buffer load and decode only for taps on the plane. Same weights and sums (a skipped tap added 0).
        if (qd <= 0) continue;
        float3 D, Dx, Dy;
        mPixelRay(float2(uint2(q)) + 0.5, D, Dx, Dy);
        const float3 qp = g_cameraPosition + D * linearDepth(qd);
        const float plane = saturate(1 - abs(dot(n, qp - p)) / planeTolerance);
        if (plane <= 0) continue;
        const float3 qv = -normalize(D);
        const float3 qm = mNormalTowardsViewer(decodeGBuffer(gbuffer.Load(int3(q, 0))).normal, qv);
        const float3 qn = dot(qm, qv) > 0 ? qm : -qm;
        float agree = saturate(dot(n, qn));
        agree *= agree;  // ^2
        agree *= agree;  // ^4
        agree *= agree;  // ^8
        agree *= agree;  // ^16
        agree *= agree;  // ^32
        const float w = exp(-2 * u) * (plane * plane) * agree;  // exp(-2 (r / R)^2), u = (r / R)^2
        sum += w * e.rgb;
        weight += w;
    }
    output[pixel] = weight > 0 ? float4(sum / weight * occlusion, 1) : float4(centre.rgb * occlusion, centre.a);
}
