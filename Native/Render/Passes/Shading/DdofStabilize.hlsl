// unx-kernel: cs_6_6 main
// Diaphragm depth of field, the temporal prefilter of the gather's input (DiaphragmDof.cpp; the reference's
// temporal anti-aliasing pass in its DOF configuration, TemporalAA.usf TAA_PASS_CONFIG 5 at the high quality): the
// half-resolution colour and radius of a jittered view accumulated onto the unjittered half-resolution grid, so the
// gathers see neither the jitter nor a highlight that is there one frame in four - a flicker a wide bokeh would turn
// into a flickering disc. Runs only for a jittered (temporally upscaled) view.
// Per pixel:
//   filtered   the plus-shaped neighbourhood under a Gaussian about the unjittered centre (exp(-2.29 d^2)), a
//              neighbour weighted down by how far behind the centre's radius it is; the radius is the centre's;
//   history    the previous output at the point the nearest surface of the pixel's surroundings was, bilinear (the
//              reference: bicubic). That surface's vector is the upscale's (shading.dof_diaphragm_prefilter_velocity;
//              m.upscale.motion, made before this pass: a moving object, a deforming one and the layers over the opaque
//              surface move as themselves, and the nearest surface is found in the depth those vectors are of - the
//              reference reads its velocity buffer the same way). Without it: the camera's motion at the view's
//              depth, a moving object's history held by the box below;
//   box        the 3 x 3 neighbourhood's minimum and maximum in YCoCg and of the radius: the history is clamped to it;
//   blend      the current frame's weight (P[2].z; the reference's r.TemporalAACurrentFrameWeight 0.04), towards 0.2
//              over 40 pixels of motion, at least 0.01 luma(history) / |luma(filtered) - luma(history)|; weighted by
//              1 / (luma + 4) each side (a highlight does not outweigh its surroundings).
// P[0] = { setup SRV (RGBA16F: rgb, a = radius), history SRV, depth SRV (full resolution), output UAV }
// P[1] = { half width, half height, full width, full height }
// P[2] = { asuint(jitter x), asuint(jitter y) (full-resolution pixels), asuint(current frame weight), flags (1: the
//          history holds the previous frame, 2: P[7] has the vectors) }, P[3..6] = rows of the previous unjittered
//          view-projection
// P[7] = { motion SRV (RG32F per full-resolution pixel: the unjittered UV now - the UV in the previous frame; beyond
//          1: no previous point - UpscaleMotion.hlsl), the depth of the surface each vector is of SRV (device depth), 0, 0 }
// The history is read by UV: its size need not be this frame's (dynamic resolution).
// Frame constants of the (jittered) main view.
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

float3 toYCoCg(float3 c) { return float3(dot(c, float3(1, 2, 1)), dot(c, float3(2, 0, -2)), dot(c, float3(-1, 2, -1))); }
float3 fromYCoCg(float3 c)
{
    const float y = c.x * 0.25, co = c.y * 0.25, cg = c.z * 0.25;
    return float3(y + co - cg, y + cg, y - co - cg);
}
float hdrWeight(float luma4) { return rcp(luma4 + 4.0); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float4> setup = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> previous = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    const float2 jitter = asfloat(P[2].xy);
    const int2 last = int2(size) - 1, lastFull = int2(P[1].zw) - 1;

    // the neighbourhood: the box, and the filtered colour
    const float4 centre = setup.Load(int3(id, 0));
    const float bilateral = abs(centre.a) > 1.0 ? rcp(abs(centre.a)) : 1.0;
    float3 lo = 1e30, hi = -1e30, filtered = 0;
    float cocLo = 1e30, cocHi = -1e30, weights = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float4 s = setup.Load(int3(clamp(int2(id) + int2(x, y), 0, last), 0));
            const float3 ycc = toYCoCg(s.rgb);
            lo = min(lo, ycc);
            hi = max(hi, ycc);
            cocLo = min(cocLo, s.a);
            cocHi = max(cocHi, s.a);
            if (x != 0 && y != 0) continue;
            // (this sample's centre from the output's unjittered one, in half-resolution pixels)
            const float2 d = float2(x, y) - 0.5 * jitter;
            const float w = exp(-2.29 * dot(d, d)) * saturate(1.0 - (centre.a - s.a) * bilateral);
            filtered += ycc * w;
            weights += w;
        }
    filtered *= ddofRcp(weights);

    float4 result = float4(fromYCoCg(filtered), centre.a);
    if (P[2].w & 1u)
    {
        // the nearest surface of the pixel's four and of the cross two pixels out (reversed depth: the largest)
        const int2 base = int2(2 * id);
        const bool vectors = (P[2].w & 2u) != 0;
        Texture2D<float> surface = ResourceDescriptorHeap[vectors ? P[7].y : P[0].z];
        float nearest = -1;
        int2 nearestAt = min(base, lastFull);
        [unroll] for (uint i = 0; i < 8; ++i)
        {
            const int2 at = clamp(base + (i < 4 ? kDdofSquare[i] : 2 * kDdofCross[i - 4]), 0, lastFull);
            const float d = surface.Load(int3(at, 0));
            if (d > nearest)
            {
                nearest = d;
                nearestAt = at;
            }
        }
        // where that surface's point at the pixel's centre was: its motion in full-resolution pixels
        const float2 full = float2(P[1].zw);
        float2 motion = 0;
        bool known = false;
        if (vectors)
        {
            Texture2D<float2> motions = ResourceDescriptorHeap[P[7].x];
            const float2 m = motions.Load(int3(nearestAt, 0));
            known = all(abs(m) < 1.0);
            motion = m * full;
        }
        else
        {
            const float2 here = float2(base) + 1.0;  // (the half-resolution pixel's centre in the jittered view)
            const float3 world = worldFromDepth(here - 0.5, max(nearest, 1e-6));
            const float4x4 prevViewProj = float4x4(asfloat(P[3]), asfloat(P[4]), asfloat(P[5]), asfloat(P[6]));
            const float4 clip = mul(prevViewProj, float4(world, 1));
            if (clip.w > 0)
            {
                const float2 was = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * full;
                motion = (here - jitter) - was;
                known = true;
            }
        }
        float2 historyUv = -1;
        float speed = 0;
        if (known)
        {
            speed = length(motion);
            historyUv = (float2(id) + 0.5 - 0.5 * motion) / float2(size);
        }
        if (all(historyUv > 0) && all(historyUv < 1) && all(isfinite(historyUv)))
        {
            const float4 h = previous.SampleLevel(g_linearClamp, historyUv, 0);
            const float3 history = clamp(toYCoCg(max(h.rgb, 0)), lo, hi);
            const float historyCoc = clamp(h.a, cocLo, cocHi);
            float blend = lerp(asfloat(P[2].z), 0.2, saturate(speed / 40.0));
            blend = max(blend, saturate(0.01 * history.x / max(abs(filtered.x - history.x), 1e-6)));
            float a = (1.0 - blend) * hdrWeight(history.x), b = blend * hdrWeight(filtered.x);
            const float norm = ddofRcp(a + b);
            a *= norm;
            b *= norm;
            result = float4(fromYCoCg(history * a + filtered * b), historyCoc * a + centre.a * b);
        }
    }
    output[id] = all(isfinite(result)) ? float4(max(result.rgb, 0), result.a) : float4(max(centre.rgb, 0), centre.a);
}
