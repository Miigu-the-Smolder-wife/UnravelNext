// unx-kernel: cs_6_6 main
// Screen traces of the ray-reuse pipeline's rays (ReflectionReuse.hlsli; ScreenTrace.hlsli), before the world rays, one
// thread per pixel of the view. A traced pixel's ray (the M job's: the same draw the trace pass replays) is followed
// across the depth buffer first; a certain hit whose point is in the previous frame's colour gives the job its value -
// that colour and the hit's distance - and the job is marked done (bit 31 of its entry in the job list): ReflectionTrace
// traces no world ray for it. Every other job is traced in the world as before, from its surface.
// (The reference continues a missed screen trace's world ray from where the screen trace ended; here it starts over.)
// P[0] = { modes SRV, results UAV, depth SRV, gbuffer SRV }, P[1] = { jobs UAV, HZB atlas SRV, previous colour SRV, frame }
// P[2] = { width, height, previous colour width, height }
// P[3] = { asuint(max trace distance), max iterations, asuint(relative depth thickness), asuint(exposure ratio) }
// P[4..7] = the previous view-projection of the previous colour (rows). Frame constants b1 = main view.
#include "Passes/Reflection/ReflectionReuse.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[2].xy;
    if (any(pixel >= size)) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    const uint m = modes.Load(int3(pixel, 0));
    if (reflMode(m) != REFL_M) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    float3 direction;
    float pdf;
    if (!s.valid || !reuseRay(s, pixel, P[1].w, direction, pdf)) return;
    Texture2D<float> hzb = ResourceDescriptorHeap[P[1].y];
    // off the surface by its slope across a pixel (the depth buffer's own steps must not stop the ray)
    const float pixelWorld = s.linearDepth * 2 * g_tanHalfFovY / g_viewHeight;
    const float3 origin = s.position + s.normal * (2 * pixelWorld * sqrt(max(1 - pow(dot(s.normal, s.view), 2), 0.0)) + 1e-3);
    const SctResult r = sctTrace(depth, hzb, size, origin, direction, asfloat(P[3].x), P[3].y, asfloat(P[3].z), 0);
    if (!r.hit || r.uncertain) return;
    const float3 hit = sctWorld(r.at);
    Texture2D<float4> previous = ResourceDescriptorHeap[P[1].z];
    const float4x4 prevViewProj = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
    const float noise = reuseUnit(pixel.x + pixel.y * 65536u + (P[1].w & 7u) * 0x9E3779B9u + 0x2545F491u);
    float3 radiance;
    if (!sctPreviousColour(previous, P[2].zw, prevViewProj, hit, asfloat(P[3].w), noise, radiance)) return;
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[1].x];
    const uint job = reflJob(m);
    results[job] = reflPackResult(reflStorable(radiance), distance(hit, s.position), 0);
    jobs[job] = jobs[job] | 0x80000000u;
}
