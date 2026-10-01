// unx-kernel: cs_6_6 main
// The previous frame's colour at the world rays' hits (reflection.lumen_sample_scene_color_at_hit; the ray-reuse pipeline,
// ReflectionReuse.hlsli), after the traversal and before the hit shading, one thread per pixel of the view. A traced
// pixel's world ray whose hit point is a surface the view sees - the depth buffer at the hit's pixel is within the
// relative thickness of the hit's depth, and that surface faces the camera by more than the normal threshold - takes
// the previous frame's colour there (ScreenTrace.hlsli sctPreviousColour) as the job's value: the lighting the view
// shows, where the screen trace gave up behind something in front. The ray's slot is emptied (the shading passes skip
// it) and the job's result is final (ReflectionCombine skips it). The structure of the reference's
// SampleSceneColorAtHit (ue6-main LumenScreenTracing.ush read as a reference; no code taken).
// Differences from the reference: the facing test uses the visible surface's shading normal at the hit's pixel (the
// hit's geometric normal is not known before the shading pass); no previous-depth test (ScreenTrace.hlsli).
// P[0] = { modes SRV, results UAV, depth SRV, gbuffer SRV }, P[1] = { jobs SRV, rays UAV, previous colour SRV, frame }
// P[2] = { width, height, previous colour width, height }
// P[3] = { asuint(relative depth thickness), asuint(cos of the normal threshold), asuint(GGX sampling bias), asuint(exposure ratio) }
// P[4..7] = the previous view-projection of the previous colour (rows). Frame constants b1 = main view.
#include "Passes/Reflection/ReflectionReuse.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[2].xy;
    if (any(pixel >= size)) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    const uint m = modes.Load(int3(pixel, 0));
    if (reflMode(m) != REFL_M) return;
    const uint job = reflJob(m);
    StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[1].x];
    if (jobs[job] & REFL_JOB_DONE) return;  // the screen trace's
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    const uint3 marker = results[job];
    if (marker.y != REFL_JOB_SPLIT) return;  // (left to the inline path: shaded there)
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[1].y];
    const uint slot = marker.x;
    const uint4 record = rays.Load4(reflRaysHitOffset(slot));
    if (record.x == REFL_RAY_MISS || record.x == REFL_RAY_NONE) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    float3 direction;
    float pdf;
    if (!s.valid || !reuseRay(s, pixel, P[1].w, asfloat(P[3].z), direction, pdf)) return;
    const float reach = asfloat(record.w);
    const float3 hit = reflRayOrigin(s) + direction * reach;
    const float4 at = sctProject(hit, float2(size));
    if (at.w <= 0 || any(at.xy < 0) || any(at.xy >= float2(size))) return;
    const uint2 hitPixel = uint2(at.xy);
    const float seenDevice = depth.Load(int3(hitPixel, 0));
    if (!(seenDevice > 0)) return;  // sky
    const float seen = linearDepth(seenDevice);
    if (abs(at.w - seen) >= asfloat(P[3].x) * max(seen, 1e-5)) return;  // another surface is seen there
    const ReflSurface t = reflSurface(depth, gbuffer, hitPixel);
    if (!t.valid || dot(t.view, t.normal) < asfloat(P[3].y)) return;  // seen at too steep an angle
    Texture2D<float4> previous = ResourceDescriptorHeap[P[1].z];
    const float4x4 prevViewProj = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
    const float noise = reuseUnit(pixel.x + pixel.y * 65536u + (P[1].w & 7u) * 0x9E3779B9u + 0x68E31DA4u);
    float3 radiance;
    if (!sctPreviousColour(previous, P[2].zw, prevViewProj, hit, asfloat(P[3].w), noise, radiance)) return;
    rays.Store4(reflRaysHitOffset(slot), uint4(REFL_RAY_NONE, 0, 0, 0));
    results[job] = reflPackResult(reflStorable(radiance), reach, 0);
}
