// unx-kernel: cs_6_6 main
// Motion blur (A5; COVERAGE 14.12 (2b), MotionBlur.cpp): the shutter's time integral over [t - s dt, t] (s = the shutter as a
// fraction of the frame interval: 180 degrees = 0.5; the exposure ends at the frame time, so it only interpolates between
// the previous and the current state). A surface now at q with velocity v_q (pixels per frame) swept [q - s v_q, q] during
// the exposure, so the surfaces that covered pixel p lie ahead of it along their motion: the gather samples q = p + t s v_n
// (t in [0, 1], v_n the largest velocity of the 3 x 3 tiles around, MotionTiles), one tap per time t of the exposure: what
// pixel p showed at that time is the surface now at q when p's own surface moves along the streak (its streak reaches q:
// the surface now at q was at p then), else p's own surface unless q's surface reached p then (its streak is at least
// |q - p|) and is in front of p's (depth order by linear depth, softly over 1 % of the depth). Coverage of a streak end is
// antialiased over a pixel. Every tap weighs 1/N: for a uniform motion the result is the exact box integral of the image
// along the streak (PostTests against the closed form); McGuire et al.'s 2012 cone weights made it a tent. Taps:
// ceil(streak / 2) (<= 2 px apart) up to 16, at the strata's midpoints (deterministic).
// Streaks are clamped to MOTION_TILE px (the neighbourhood the tiles cover; longer - fast rotation - goes to the
// integral-image path of 14.12 (2a)). A tile whose neighbourhood streak is under half a pixel copies its pixels.
// P[0] = { colour SRV, velocity SRV, neighbour-max SRV, depth SRV }, P[1] = { destination UAV, width, height, frame (unused) },
// P[2] = { asfloat shutter, 0, 0, 0 }; frame constants of the view (near plane).
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define MOTION_TILE 32.0f

float2 streak(float2 v, float shutter)
{
    const float2 s = v * shutter;
    const float l = length(s);
    return l > MOTION_TILE ? s * (MOTION_TILE / l) : s;
}
// a streak of this length reaches a point d pixels away (antialiased over one pixel)
float reaches(float length, float d) { return saturate(length - d + 0.5f); }
// a at depth za in front of b at zb (linear depths), softly over 1 % of the depth
float inFront(float za, float zb) { return saturate(1.0f - (za - zb) / (0.01f * max(za, zb))); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].yz;
    if (any(id >= size)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float2> velocity = ResourceDescriptorHeap[P[0].y];
    Texture2D<float2> neighbour = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const float shutter = asfloat(P[2].x);
    const float4 cp = colour.Load(int3(id, 0));
    const float2 vn = streak(neighbour.Load(int3(id / 32u, 0)), shutter);
    const float ln = length(vn);
    if (ln < 0.5f)
    {
        output[id] = cp;
        return;
    }
    const float2 p = float2(id) + 0.5f;
    const float zp = g_nearPlane / max(depth.Load(int3(id, 0)), 1e-30f);  // reversed Z: device 0 (sky) -> far
    const float lp = length(streak(velocity.Load(int3(id, 0)), shutter));
    const uint taps = clamp((uint)ceil(ln * 0.5f), 1u, 16u);
    float3 sum = 0;
    for (uint i = 0; i < taps; ++i)
    {
        // the midpoint rule over the exposure (error O(h^2 f''); a shared random offset shifts the whole rectangle rule by
        // up to h/2 (f(end) - f(start)) / L, independent offsets add noise: both measured larger, PostTests)
        const float t = ((float)i + 0.5f) / (float)taps;
        const float2 q = p + t * vn;
        const int2 qi = clamp(int2(floor(q)), int2(0, 0), int2(size) - 1);
        const float d = t * ln;
        const float zq = g_nearPlane / max(depth.Load(int3(qi, 0)), 1e-30f);
        const float lq = length(streak(velocity.Load(int3(qi, 0)), shutter));
        const float takeQ = max(reaches(lp, d), reaches(lq, d) * inFront(zq, zp));
        // the colour at the tap's exact position (bilinear: taps 2 px apart share one sub-pixel offset, which nearest texels
        // would turn into a shift of the whole streak); depth and velocity classify, so they are the texel's
        sum += lerp(cp.rgb, colour.SampleLevel(g_linearClamp, q / float2(size), 0).rgb, takeQ);
    }
    output[id] = float4(sum / (float)taps, cp.a);
}
