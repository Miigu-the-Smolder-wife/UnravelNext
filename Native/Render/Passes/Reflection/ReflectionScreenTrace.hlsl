// unx-kernel: cs_6_6 main
// Screen traces of the ray-reuse pipeline's rays (ReflectionReuse.hlsli; ScreenTrace.hlsli), before the world rays, one
// thread per pixel of the view. A traced pixel's ray (the M job's: the same draw the trace pass replays) is followed
// across the depth buffer first; a certain hit whose point is in the previous frame's colour gives the job its value -
// that colour and the hit's distance - and the job is marked done (bit 31 of its entry in the job list): ReflectionTrace
// traces no world ray for it. Every other job's world ray starts where its screen trace ended in front of the scene
// (reflection.lumen_screen_trace_continue: results[job].x = that distance less the pull-back; the space the screen
// trace crossed in front of the depth buffer is empty) - or at its surface (0) with the switch off.
// P[0] = { modes SRV, results UAV, depth SRV, gbuffer SRV }, P[1] = { jobs UAV, HZB atlas SRV, previous colour SRV, frame }
// P[2] = { width, height, previous colour width | height << 16, asuint(pull-back, m; < 0: world rays start at the surface) }
// P[3] = { asuint(max trace distance), max iterations | GGX sampling bias unorm16 << 16, asuint(relative depth thickness),
//          asuint(exposure ratio) }
// P[4..7] = the previous view-projection of the previous colour (rows). Frame constants b1 = main view.
// P[8].x = M's material word (the top layer's roughness, ReflectionInternal.hlsli g_reflWords; UNX_NONE: none),
// P[8].y != 0: the previous colour's alpha is its frame's depth (the history depth test, ScreenTrace.hlsli).
#include "Passes/Reflection/ReflectionReuse.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[2].xy;
    if (any(pixel >= size)) return;
    g_reflWords = P[8].x;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    const uint m = modes.Load(int3(pixel, 0));
    if (reflMode(m) != REFL_M) return;
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    const uint job = reflJob(m);
    if (job == REFL_NO_JOB) return;  // (reflection.lumen_downsample: the block's ray is another pixel's)
    results[job] = uint3(0, 0, 0);  // (the world ray's start: its surface, unless set below)
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    float3 direction;
    float pdf;
    if (!s.valid || !reuseRay(s, pixel, P[1].w, (P[3].y >> 16) / 65535.0, direction, pdf)) return;
    Texture2D<float> hzb = ResourceDescriptorHeap[P[1].y];
    // off the surface by its slope across a pixel (the depth buffer's own steps must not stop the ray)
    const float pixelWorld = s.linearDepth * 2 * g_tanHalfFovY / g_viewHeight;
    const float3 origin = s.position + s.normal * (2 * pixelWorld * sqrt(max(1 - pow(dot(s.normal, s.view), 2), 0.0)) + 1e-3);
    const SctResult r = sctTrace(depth, hzb, size, origin, direction, asfloat(P[3].x), P[3].y & 0xFFFFu, asfloat(P[3].z), 0);
    const float3 hit = sctWorld(r.at);
    // The world ray's start if this trace gives no value: the walk's end (a clean miss: 2 cm further, the end's distance
    // is not exact) less the pull-back.
    const float pullback = asfloat(P[2].w);
    const float resume = pullback >= 0 && r.iterations > 0 ? max(distance(hit, s.position) + (r.hit ? 0.0 : 0.02) - pullback, 0.0) : 0.0;
    results[job] = uint3(asuint(resume), 0, 0);
    if (!r.hit || r.uncertain) return;
    Texture2D<float4> previous = ResourceDescriptorHeap[P[1].z];
    const float4x4 prevViewProj = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
    const float noise = reuseUnit(pixel.x + pixel.y * 65536u + (P[1].w & 7u) * 0x9E3779B9u + 0x2545F491u);
    float3 radiance;
    if (!sctPreviousColour(previous, uint2(P[2].z & 0xFFFFu, P[2].z >> 16), prevViewProj, hit, asfloat(P[3].w), noise, radiance, P[8].y != 0)) return;
    RWStructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[1].x];
    // (the previous colour holds neither the air nor the fog: the fog along this ray is the ray's own)
    radiance = fogOverRay((float2(pixel) + 0.5) / float2(size), s.linearDepth, origin, direction, distance(hit, s.position), radiance);
    results[job] = reflPackResult(reflStorable(radiance), distance(hit, s.position), 0);
    jobs[job] = jobs[job] | 0x80000000u;
}
