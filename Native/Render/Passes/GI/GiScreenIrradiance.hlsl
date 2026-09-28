// unx-kernel: cs_6_6 main
// unx-variants: SPLIT=0,1
// r.gi.screen: M's per-pixel GI cache irradiance (front side) as a pass of its own, written to view.giIrradiance for M's
// shading kernel to read once (R_STATUS_KO.md 0, GI tile path verdict: the lookup costs 2.44 ms at 4K in a kernel of its
// own occupancy [measured], against ~3.4 ms inside M's shading kernel [expected]). The same function on the same inputs
// as M (GiScreenInputs.hlsli), so the only difference is the storage: RGBA16F, rgb = irradiance x the view's exposure
// (relative rounding <= 2^-11; pre-exposed so no value leaves the half range), a = 1 where the cache had data (weight
// > 0), 0 where M keeps the screen probes' irradiance. Pixels without a surface get 0.
// The cache's anchor visibility (gi.anchor_visibility, giScreenSeen) re-weights each level's corners where they are all
// converged. (A quad-shared form with batched corner loads, plus the visibility loads, measured 1.44 ms against this
// form's 0.77 without visibility at 1440p [measured, 20379fb, RTX 4080]: the shared form is removed.)
//
// SPLIT=0: the lookup for every pixel (the default: gi.screen_update_frames = 1). SPLIT=1, frame split
// (gi.screen_update_frames = N > 1, the main view with V's vis buffer; user decision 2026-09-28; off by default since
// 368e103 [measured]: r.gi.screen 0.89 -> 0.98 ms at internal 1440p - the reuse tests below compare each neighbour's
// G-buffer normal, which is the normal-mapped shading normal, so on textured surfaces most pixels fail the test and pay
// the attempt and the lookup; the lookup's value depends on that exact normal, so a neighbour's value is not the pixel's
// either): the view's
// 8 x 8 tiles take turns - a tile looks the cache up in one frame of N (2 x 2 tile Bayer order, whole groups, so the
// waves that reuse skip the lookup). A pixel of a tile that does not look up this frame takes its own surface point's
// value of the previous frame: the point's exact motion (GiScreenHistory.hlsli giPreviousSurface: the vis buffer's
// triangle in its previous-tick vertices) into the previous view, bilinear over the 4 previous pixels around it. Each of
// the 4 must hold the same surface: its stored point (previous depth, unprojected) within 2 pixel footprints over the
// view's cosine of the point's previous position and near its plane, its normal in the same cache normal class as the point's normal now and within 8 deg of the
// point's previous normal, the same data flag, and a value at most N - 2 frames old; the point itself may have moved at
// most a quarter footprint since the previous frame. Otherwise (disocclusion, edges, moving objects, the first frame)
// the pixel looks the cache up. A value is therefore at most N - 1 frames old (the lighting lag of the split) and is the
// lookup's value at the same surface point up to the bilinear resampling of a field that varies over cells of >= 7 px
// (cache_cell_angle_deg at 960 px height). Values are rescaled by this frame's exposure over the previous one.
// Per pixel the previous-frame record (keys) holds the device depth, the normal (octahedral 15 + 15 bits) and the
// value's age (2 bits); 0 = no surface.
// P[0] = { cache SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { width, height, split flags, exposure ratio (float) }
// split flags: bit 0 the keys are written (split on), bit 1 the previous frame's value and keys are valid, bits 2-3 this
// frame's phase (frame % N), bits 4-6 N. P[2] = { previous keys SRV (Texture2D<uint2>), keys UAV, previous value SRV,
// vis id SRV }, P[3].x visible clusters SRV, P[4..7] the previous frame's inverse view-projection (rows, the jittered one
// of the previous frame's main view). b1 = the main view.
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

#if SPLIT
#define GI_SPLIT_KEYS 1u
#define GI_SPLIT_PREVIOUS 2u

uint giScreenPackNormal(float3 n, uint age)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : octWrap(n.xy);
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 16383.0));
    return (uint(q.x) & 0x7FFFu) | ((uint(q.y) & 0x7FFFu) << 15) | (age << 30);
}
float3 giScreenUnpackNormal(uint packed)
{
    const float2 e = float2(int2(packed << 17, packed << 2) >> 17) / 16383.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = octWrap(n.xy);
    return normalize(n);
}

// The previous frame's value at the pixel's surface point, or false (the pixel looks the cache up).
bool giScreenReuse(uint2 pixel, float3 worldPos, float3 nv, float footprint, uint maxAge, out float4 value, out uint age)
{
    value = 0;
    age = 0;
    float3 prevP, prevN;
    uint instance;
    giPreviousSurface(P[2].w, P[3].x, pixel, worldPos, nv, prevP, prevN, instance);
    if (instance == 0) return false;  // no vis id: no exact motion
    if (!(distance(worldPos, prevP) <= 0.25 * footprint)) return false;  // moved: the cache's field is the new position's
    const float2 size = float2(P[1].xy);
    float2 prevPixel;
    float prevDepth;
    if (!giPreviousPixel(prevP, size, prevPixel, prevDepth)) return false;
    const float2 x = prevPixel - 0.5;
    const int2 i0 = int2(floor(x));
    if (any(i0 < 0) || any(i0 + 1 >= int2(P[1].xy))) return false;
    const float2 f = x - floor(x);
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[2].x];
    Texture2D<float4> values = ResourceDescriptorHeap[P[2].z];
    const float4x4 invPrev = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
    const uint nc = giNormalClass(nv);
    // The taps lie within 1.5 pixels of the point: along the surface up to 2 footprints over the cosine of the view
    // (a floor at a grazing view spreads a pixel over several footprints), off the point's plane at most a footprint
    // plus half their distance (the G-buffer normal is the shading normal).
    const float reach = 2 * footprint / max(abs(dot(nv, normalize(g_cameraPosition - worldPos))), 0.25);
    float4 sum = 0;
    uint oldest = 0;
    float flag = -1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 q = i0 + int2(k & 1, k >> 1);
        const uint2 key = keys.Load(int3(q, 0));
        if (key.x == 0) return false;
        const uint a = key.y >> 30;
        if (a >= maxAge) return false;
        const float2 ndc = float2((q.x + 0.5) / size.x * 2 - 1, 1 - (q.y + 0.5) / size.y * 2);
        const float4 h = mul(invPrev, float4(ndc, asfloat(key.x), 1));
        const float3 s = h.xyz / h.w;
        const float d = distance(s, prevP);
        if (!(d <= reach) || !(abs(dot(prevN, s - prevP)) <= footprint + 0.5 * d)) return false;
        const float3 n = giScreenUnpackNormal(key.y);
        if (giNormalClass(n) != nc || dot(n, prevN) < 0.99) return false;
        const float4 v = values.Load(int3(q, 0));
        if (flag >= 0 && v.a != flag) return false;
        flag = v.a;
        const float w = (k & 1 ? f.x : 1 - f.x) * (k & 2 ? f.y : 1 - f.y);
        sum += w * v;
        oldest = max(oldest, a);
    }
    value = float4(sum.rgb * asfloat(P[1].w), flag);
    age = oldest + 1;
    return true;
}

#endif

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint2 tile : SV_GroupID)
{
    if (any(pixel >= P[1].xy)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    const float depthValue = depth.Load(int3(pixel, 0));
    const uint split = P[1].z;
    float3 worldPos, nv;
    float4 result = 0;
    uint age = 0;
    const bool surface = giScreenInputs(pixel, depthValue, gbuffer.Load(int3(pixel, 0)), worldPos, nv);
    bool lookup = surface;
#if SPLIT
    if (surface && (split & GI_SPLIT_PREVIOUS) != 0)
    {
        const uint n = (split >> 4) & 7u, phase = (split >> 2) & 3u;
        const uint bayer = (tile.x & 1u) * 2u + (tile.y & 1u) * 3u - (tile.x & tile.y & 1u) * 4u;  // (0,0) 0, (1,0) 2, (0,1) 3, (1,1) 1
        if (bayer % n != phase)
        {
            const float footprint = 2 * linearDepth(depthValue) * g_tanHalfFovY / g_viewHeight;
            lookup = !giScreenReuse(pixel, worldPos, nv, footprint, n - 1, result, age);
        }
    }
#endif
    if (lookup)
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
        float weight;
        const float3 e = giCacheIrradianceScreen(cache, giHeader(cache), worldPos, nv, weight);
        result = weight > 0 ? float4(e * g_exposure, 1) : float4(0, 0, 0, 0);
    }
    output[pixel] = result;
#if SPLIT
    if (split & GI_SPLIT_KEYS)
    {
        RWTexture2D<uint2> keys = ResourceDescriptorHeap[P[2].y];
        keys[pixel] = surface ? uint2(asuint(depthValue), giScreenPackNormal(nv, age)) : uint2(0, 0);
    }
#endif
}
