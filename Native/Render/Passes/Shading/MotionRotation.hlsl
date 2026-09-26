// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2
// Motion blur, rotation stage (COVERAGE 14.12 (2a), MotionBlur.cpp): the camera's rotation during the exposure is exact for
// every depth (no parallax) - pixel p, looking along d_p now, saw at the exposure time tau (0 = the frame time) the
// direction Q^tau d_p of this frame's image (Q = R_cur R_prev^T, angle phi about axis a): its value is the mean of the
// image over the arc [lambda_p, lambda_p + s phi] of its latitude circle about a. The frame is resampled onto
// (lambda, beta) about a (texels of the centre pixel's angle, off-screen directions weight 0), prefix-summed along lambda
// per row, and each pixel reads its arc with two taps: (S(lambda + s phi) - S(lambda)) / (the weights' difference).
//   STEP=0: the map (RGBA32F: rgb, weight) from the image (bilinear at each texel's direction);
//   STEP=1: inclusive prefix sums along each row (one group per row, 1024 threads, deterministic order);
//   STEP=2: each pixel's arc mean (bilinear on the prefix map); a pixel whose arc left the image keeps its value where
//           no sample remains.
// A first-person view model (INSTANCE_VIEW_MODEL, A12) turns with the camera, so it is no part of the rotated world: its
// texels have weight 0 in the map (a direction behind it is unknown now, like one off the image) and its pixels keep
// their value, as do the pixels with it in their 3 x 3 neighbourhood (their edge composite may hold a share of it).
// P[0] = { image SRV, map UAV / SRV, output UAV, vis id SRV or UNX_NONE (no view model this frame) }, P[1] = { map width,
// map height, width, height }, P[2] = asfloat { lambda0, beta0, texel angle, arc s phi }, P[3..5] = asfloat axis a, e1, e2
// (view space, xyz), P[3].w = visible clusters SRV (with a vis id SRV); frame constants of the view (projection).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

bool isViewModel(int2 pixel)
{
    if (P[0].w == UNX_NONE) return false;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[0].w];
    const uint visId = vis.Load(int3(pixel, 0));
    if (visId == VIS_NONE) return false;
    return (loadInstance(loadVisibleCluster(P[3].w, visVisibleCluster(visId)).instance).flags & INSTANCE_VIEW_MODEL) != 0;
}

float3 axisOf(uint k) { return asfloat(uint3(P[3 + k].x, P[3 + k].y, P[3 + k].z)); }

// view-space unit direction of a pixel position (pixel units; the view's projection has no shear)
float3 pixelDirection(float2 pixel)
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    return normalize(float3((ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0], (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1], -1));
}

#if STEP == 0
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[1].xy)) return;
    Texture2D<float4> image = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> map = ResourceDescriptorHeap[P[0].y];
    const float texel = asfloat(P[2].z);
    const float lambda = asfloat(P[2].x) + ((float)id.x + 0.5f) * texel, beta = asfloat(P[2].y) + ((float)id.y + 0.5f) * texel;
    const float3 a = axisOf(0), e1 = axisOf(1), e2 = axisOf(2);
    const float3 d = cos(beta) * (cos(lambda) * e1 + sin(lambda) * e2) + sin(beta) * a;
    float4 value = 0;
    if (d.z < -1e-4f)
    {
        const float2 ndc = float2(d.x * g_proj[0][0] / -d.z - g_proj[0][2] + g_proj[0][3], d.y * g_proj[1][1] / -d.z - g_proj[1][2] + g_proj[1][3]);
        const float2 pixel = float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight);
        if (all(pixel >= 0) && all(pixel <= float2(g_viewWidth, g_viewHeight)))
        {
            if (P[0].w == UNX_NONE) value = float4(image.SampleLevel(g_linearClamp, pixel / float2(g_viewWidth, g_viewHeight), 0).rgb, 1);
            else
            {
                // the same bilinear taps (clamped) without the view model's texels: premultiplied colour and the kept weight
                const float2 t = pixel - 0.5f;
                const int2 i0 = int2(floor(t));
                const float2 f = t - float2(i0);
                const int2 last = int2(g_viewWidth, g_viewHeight) - 1;
                [unroll] for (uint k = 0; k < 4; ++k)
                {
                    const int2 o = int2(k & 1, k >> 1);
                    const int2 texelAt = clamp(i0 + o, int2(0, 0), last);
                    const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
                    if (w > 0 && !isViewModel(texelAt)) value += float4(w * image.Load(int3(texelAt, 0)).rgb, w);
                }
            }
        }
    }
    map[id] = value;
}
#elif STEP == 1
groupshared float4 gs_sum[1024];

[numthreads(1024, 1, 1)]
void main(uint3 gid : SV_GroupID, uint t : SV_GroupIndex)
{
    RWTexture2D<float4> map = ResourceDescriptorHeap[P[0].y];
    const uint width = P[1].x, row = gid.x;
    float4 carry = 0;
    for (uint c0 = 0; c0 < width; c0 += 1024)
    {
        const uint x = c0 + t;
        gs_sum[t] = x < width ? map[uint2(x, row)] : 0;
        GroupMemoryBarrierWithGroupSync();
        for (uint s = 1; s < 1024; s <<= 1)  // Hillis-Steele: a fixed combination order (deterministic)
        {
            const float4 o = t >= s ? gs_sum[t - s] : 0;
            GroupMemoryBarrierWithGroupSync();
            gs_sum[t] += o;
            GroupMemoryBarrierWithGroupSync();
        }
        if (x < width) map[uint2(x, row)] = carry + gs_sum[t];
        carry += gs_sum[1023];
        GroupMemoryBarrierWithGroupSync();
    }
}
#else
// the prefix map at a fractional position (u along lambda in texels from the row's start, v along beta): the inclusive sum
// of texel i is the integral up to the texel's end, u = i + 1 (0 at u = 0), linear between; rows at their centres
float4 prefixAt(Texture2D<float4> map, float u, float v)
{
    const uint2 size = P[1].xy;
    const float fu = u - 1.0f, fv = clamp(v - 0.5f, 0.0f, (float)size.y - 1);
    const int u0 = (int)floor(fu), v0 = (int)floor(fv);
    const float wu = fu - u0, wv = fv - v0;
    float4 r = 0;
    [unroll] for (int j = 0; j < 2; ++j)
    {
        const int row = min(v0 + j, (int)size.y - 1);
        const float4 a = u0 < 0 ? 0 : map.Load(int3(min(u0, (int)size.x - 1), row, 0));
        const float4 b = u0 + 1 < 0 ? 0 : map.Load(int3(min(u0 + 1, (int)size.x - 1), row, 0));
        r += (j == 0 ? 1 - wv : wv) * lerp(a, b, wu);
    }
    return r;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[1].zw)) return;
    Texture2D<float4> image = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> map = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    const float4 own = image.Load(int3(id, 0));
    // a view-model pixel, or one with a view-model pixel in its 3 x 3 neighbourhood: M's edge composite builds an edge
    // pixel from that neighbourhood (Edge.hlsli), so its value may hold a share of the view model, which must not move
    if (P[0].w != UNX_NONE)
    {
        const int2 last = int2(P[1].zw) - 1;
        bool keep = false;
        [unroll] for (uint k = 0; k < 9 && !keep; ++k) keep = isViewModel(clamp(int2(id) + int2(int(k % 3) - 1, int(k / 3) - 1), int2(0, 0), last));
        if (keep)
        {
            output[id] = own;
            return;
        }
    }
    const float3 d = pixelDirection(float2(id) + 0.5f);
    const float3 a = axisOf(0), e1 = axisOf(1), e2 = axisOf(2);
    const float texel = asfloat(P[2].z), arc = asfloat(P[2].w);
    const float lambda = atan2(dot(d, e2), dot(d, e1)), beta = asin(clamp(dot(d, a), -1.0f, 1.0f));
    const float u = (lambda - asfloat(P[2].x)) / texel, v = (beta - asfloat(P[2].y)) / texel;
    const float4 s = prefixAt(map, u + arc / texel, v) - prefixAt(map, u, v);
    // the arc's samples that fell on the image (its span in texels is arc / texel); none: the pixel keeps its value
    output[id] = s.w > 0.5f ? float4(s.rgb / s.w, own.a) : own;
}
#endif
