// Screen-space ray tracing shared by the reflection rays and GI's probe rays (reflection.lumen_screen_traces; R: its probe
// trace): a ray is first followed across the depth buffer; where it meets a visible surface the hit takes the previous
// frame's scene colour and no world ray is traced. The structure of Unreal's Lumen screen traces (ue6-main HZBTracing.ush,
// LumenScreenTracing.ush read as a reference; no code taken).
//   HZB        ReflectionHzb.hlsl: levels 1..SCT_LEVELS of the view's depth, each texel the closest depth (the largest
//              device depth: reversed Z) of 2 x 2 texels of the level below; level 0 is the depth buffer itself. The levels
//              sit side by side in one R32F texture (sctLevelOrigin): level 1 at (0, 0), levels 2.. stacked to its right.
//   trace      sctTrace: the ray in (pixel x, pixel y, device depth) - straight there - walks the cells it crosses. In a
//              cell whose closest depth is behind the ray it skips to the cell's far edge and goes one level up; else it
//              goes one level down; below level 0 it has met the depth buffer: a hit when the ray is not deeper behind
//              the surface than relativeThickness x the surface's depth (else it went behind something: no hit, and the
//              point where it was last in front is returned for a world ray to go on from).
//   colour     sctPreviousColour: the hit point in the previous frame's colour (ViewResources::prevSceneColor), unless it
//              lies near the screen's edge (a dithered fade) or behind the previous camera.
//              With output.screen_trace_source = 0 the previous colour's alpha holds that frame's device depth
//              (UpscaleSceneKeep.hlsl) and the hit takes the reference's history depth test: a point that was hidden
//              in the previous frame (its depth then against the depth buffer's there) has no colour to take.
//   distant    sctDistantTrace: the reference's distant screen traces (LumenScreenTracing.ush DistantScreenTrace) - a
//              linear walk of the depth buffer for a world ray that left the ray scene, over the stretch past the rays'
//              end (there: past the ray tracing culling radius).
// Not here (a difference from the reference): moving objects' motion at the hit (the hit point is taken as still).
#ifndef UNX_SCREEN_TRACE_HLSLI
#define UNX_SCREEN_TRACE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define SCT_LEVELS 6u

uint2 sctLevelSize(uint2 size, uint level) { return max((size + ((1u << level) - 1u)) >> level, uint2(1, 1)); }
uint2 sctLevelOrigin(uint2 size, uint level)
{
    if (level <= 1) return uint2(0, 0);
    uint y = 0;
    for (uint k = 2; k < level; ++k) y += sctLevelSize(size, k).y;
    return uint2(sctLevelSize(size, 1).x, y);
}
// The atlas texture's size for a view of 'size' pixels (C++ mirrors it: ReflectionSystem.cpp).
uint2 sctAtlasSize(uint2 size)
{
    uint tall = 0;
    for (uint k = 2; k <= SCT_LEVELS; ++k) tall += sctLevelSize(size, k).y;
    return uint2(sctLevelSize(size, 1).x + sctLevelSize(size, 2).x, max(sctLevelSize(size, 1).y, tall));
}

float sctClosestDepth(Texture2D<float> depth, Texture2D<float> hzb, uint2 size, int level, float2 p)
{
    if (level <= 0) return depth.Load(int3(clamp(int2(floor(p)), int2(0, 0), int2(size) - 1), 0));
    const uint2 ls = sctLevelSize(size, (uint)level);
    const int2 c = clamp(int2(floor(p / (float)(1u << (uint)level))), int2(0, 0), int2(ls) - 1);
    return hzb.Load(int3(c + int2(sctLevelOrigin(size, (uint)level)), 0));
}

// A world point in (pixel x, pixel y, device depth) of the current view; w = its view depth (<= 0: behind the camera).
float4 sctProject(float3 world, float2 size)
{
    const float4 clip = mul(g_viewProj, float4(world, 1));
    const float w = max(clip.w, 1e-6);
    return float4((clip.x / w * 0.5 + 0.5) * size.x, (0.5 - clip.y / w * 0.5) * size.y, clip.z / w, clip.w);
}

struct SctResult
{
    bool hit;            // the ray met a visible surface
    bool uncertain;      // ... but a thin feature lies just past it (thicknessSteps)
    float3 at;           // where the walk ended (pixel x, y, device depth): the hit, or the last place in front of the scene
    uint iterations;
};

// The world position of a trace point.
float3 sctWorld(float3 at) { return worldFromDepth(at.xy - 0.5, max(at.z, 1e-30)); }

SctResult sctTrace(Texture2D<float> depth, Texture2D<float> hzb, uint2 size, float3 origin, float3 direction, float maxDistance, uint maxIterations,
                   float relativeThickness, uint thicknessSteps)
{
    SctResult o;
    o.hit = o.uncertain = false;
    o.iterations = 0;
    const float2 sizeF = float2(size);
    const float4 startP = sctProject(origin, sizeF);
    o.at = startP.xyz;
    if (startP.w <= 0 || any(startP.xy < 0) || any(startP.xy >= sizeF)) return o;
    // the end: at most maxDistance, kept in front of the camera, then cut where the segment leaves the screen
    const float wPerUnit = mul(g_viewProj, float4(direction, 0)).w;
    float reach = maxDistance;
    if (wPerUnit < 0) reach = min(reach, -0.99 * startP.w / wPerUnit);
    const float3 start = startP.xyz;
    float3 end = sctProject(origin + direction * reach, sizeF).xyz;
    float3 d = end - start;
    float cut = 1;
    if (d.x > 0) cut = min(cut, (sizeF.x - start.x) / d.x);
    else if (d.x < 0) cut = min(cut, -start.x / d.x);
    if (d.y > 0) cut = min(cut, (sizeF.y - start.y) / d.y);
    else if (d.y < 0) cut = min(cut, -start.y / d.y);
    if (d.z < 0) cut = min(cut, -start.z / d.z);  // (device depth 0: infinitely far)
    d *= saturate(cut);
    end = start + d;
    if (dot(d.xy, d.xy) < 1e-6) return o;  // (a ray along the view ray: nothing to walk)
    const float3 inv = float3(abs(d.x) > 1e-12 ? 1 / d.x : 1e12, abs(d.y) > 1e-12 ? 1 / d.y : 1e12, abs(d.z) > 1e-12 ? 1 / d.z : 1e12);
    const float2 edge = float2(d.x < 0 ? -0.005 : 1.005, d.y < 0 ? -0.005 : 1.005);
    // out of the start pixel without a test (the ray's own surface)
    float t;
    {
        const float2 tt = (floor(start.xy) + edge - start.xy) * inv.xy;
        t = min(tt.x, tt.y);
    }
    float3 p = start + t * d;
    float lastFront = 0;
    int level = 0;
    [loop] while (level >= 0 && o.iterations < maxIterations && t < 1)
    {
        const float cell = (float)(1u << (uint)level);
        const float closest = sctClosestDepth(depth, hzb, size, level, p.xy);
        float3 tt = (float3((floor(p.xy / cell) + edge) * cell, closest) - start) * inv;
        tt.z = d.z < 0 ? tt.z : 1.0;  // (a ray coming nearer never crosses the cell's closest depth from the front)
        const bool front = p.z > closest;  // reversed Z: larger = nearer
        float next = min(min(tt.x, tt.y), tt.z);
        const bool skipped = front && next != tt.z;
        if (skipped) lastFront = next;
        if (front) t = next;
        p = start + t * d;
        level = min(level + (skipped ? 1 : -1), (int)SCT_LEVELS);
        ++o.iterations;
    }
    p = start + (o.iterations > 0 ? min(t, 1.0) : 0.0) * d;
    if (level < 0 && t < 1)
    {
        const float surface = linearDepth(max(sctClosestDepth(depth, hzb, size, 0, p.xy), 1e-30));
        o.hit = linearDepth(max(p.z, 1e-30)) - surface < relativeThickness * max(surface, 1e-5);
        if (!o.hit) p = start + lastFront * d;  // it went behind the surface: back to where it was last in front
    }
    if (o.hit && thicknessSteps > 0)
    {
        // a few steps on along the ray at level 1: back in front of the scene = a feature thinner than the test saw
        const float2 perStep = normalize(d.xy) * 2.0;
        const float tStep = length(perStep) / max(length(d.xy), 1e-6);
        [loop] for (uint i = 1; i <= thicknessSteps; ++i)
        {
            const float3 q = p + d * (tStep * i);
            if (any(q.xy < 0) || any(q.xy >= sizeF)) break;
            if (q.z > sctClosestDepth(depth, hzb, size, 1, q.xy)) o.uncertain = true;
        }
    }
    o.at = p;
    return o;
}

// Distant screen traces (reflection.lumen_distant_screen_traces): what the ray scene does not hold - the view's surfaces
// past the rays' length - can still be on screen. The ray from 'origin' (the world ray's end) is walked across the depth
// buffer for at most maxDistance in SCT_DISTANT_STEPS equal steps of its screen segment (no pyramid: 16 loads), the
// samples offset by stepOffset of a step (the pixel's noise + the bias). A sample is a hit when the ray lies behind the
// depth buffer there by less than twice the tolerance - per step the larger of the ray's own change of device depth and
// slopeTolerance x the device depth a point as far again along the view would lose (the reference's slope compare
// tolerance) - and the buffer holds a surface (not the far plane). A start outside the view meets nothing the screen
// holds. hitWorld: the depth buffer's point at the hit's pixel.
#define SCT_DISTANT_STEPS 16u
bool sctDistantTrace(Texture2D<float> depth, uint2 size, float3 origin, float3 direction, float maxDistance, float slopeTolerance, float stepOffset, out float3 hitWorld)
{
    hitWorld = 0;
    const float2 sizeF = float2(size);
    const float4 startP = sctProject(origin, sizeF);
    if (!(startP.w > 0) || any(startP.xy < 0) || any(startP.xy >= sizeF) || !(maxDistance > 0)) return false;
    const float wPerUnit = mul(g_viewProj, float4(direction, 0)).w;
    float reach = maxDistance;
    if (wPerUnit < 0) reach = min(reach, -0.95 * startP.w / wPerUnit);  // (kept in front of the camera)
    const float3 start = startP.xyz;
    float3 d = sctProject(origin + direction * reach, sizeF).xyz - start;
    float cut = 1;
    if (d.x > 0) cut = min(cut, (sizeF.x - start.x) / d.x);
    else if (d.x < 0) cut = min(cut, -start.x / d.x);
    if (d.y > 0) cut = min(cut, (sizeF.y - start.y) / d.y);
    else if (d.y < 0) cut = min(cut, -start.y / d.y);
    d *= saturate(cut);
    // (device depth x view depth is constant: the device depth of a point 'reach' deeper along the view from the start)
    const float deeper = start.z * startP.w / (startP.w + reach);
    const float tolerance = max(abs(d.z), (start.z - deeper) * slopeTolerance) / SCT_DISTANT_STEPS;
    const float3 perStep = d / SCT_DISTANT_STEPS;
    [loop] for (uint i = 0; i < SCT_DISTANT_STEPS; ++i)
    {
        const float3 p = start + perStep * (stepOffset + (float)(i + 1));
        if (any(p.xy < 0) || any(p.xy >= sizeF) || !(p.z > 0)) break;
        const float seen = depth.Load(int3(int2(p.xy), 0));
        if (!(seen > 0)) continue;  // (the far plane: the sky, nothing to meet)
        // reversed Z: the ray behind the surface = its device depth is the smaller one
        const float behind = seen - p.z;
        if (behind > 0 && behind < 2 * tolerance)
        {
            hitWorld = sctWorld(float3(p.xy, seen));
            return true;
        }
    }
    return false;
}

// How much of a hit near the screen's edge is kept (1 inside, 0 at the edge), for a dithered rejection.
float sctVignette(float2 ndc)
{
    const float2 v = saturate(abs(ndc) * 5 - 4);
    return saturate(1 - dot(v, v));
}

// The previous frame's colour at a world point (nits): prevColour = ViewResources::prevSceneColor of prevSize pixels,
// prevViewProj = FrameContext::upscale.prevViewProj, exposureRatio = FrameContext::upscale.exposureRatio, noise in [0, 1).
// False when the point was outside that frame or too near its edge (or this frame's).
// historyDepth: prevColour's alpha is that frame's device depth - the reference's history depth test (its numbers: the
// difference of device depths on its 10 cm near plane under 0.005 x (0.5 .. 2 by the noise)).
#define SCT_HISTORY_DEPTH_THICKNESS 0.005
bool sctPreviousColour(Texture2D<float4> prevColour, uint2 prevSize, float4x4 prevViewProj, float3 world, float exposureRatio, float noise, out float3 radiance,
                       bool historyDepth = false)
{
    radiance = 0;
    const float4 clip = mul(prevViewProj, float4(world, 1));
    if (!(clip.w > 0)) return false;
    const float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) >= 1)) return false;
    const float4 now = mul(g_viewProj, float4(world, 1));
    if (min(sctVignette(ndc), sctVignette(now.xy / max(now.w, 1e-6))) < noise) return false;
    const float2 at = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(prevSize) - 0.5;
    if (historyDepth)
    {
        const int2 nearest = clamp(int2(floor(at + 0.5)), int2(0, 0), int2(prevSize) - 1);
        const float seenThen = linearDepth(prevColour.Load(int3(nearest, 0)).a);  // (sky: very far)
        if (abs(0.1 / seenThen - 0.1 / clip.w) >= SCT_HISTORY_DEPTH_THICKNESS * lerp(0.5, 2.0, noise)) return false;
    }
    const int2 i0 = int2(floor(at));
    const float2 f = at - floor(at);
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = clamp(i0 + o, int2(0, 0), int2(prevSize) - 1);
        sum += prevColour.Load(int3(q, 0)).rgb * ((o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y));
    }
    radiance = max(sum, 0.0) * (exposureRatio / max(g_exposure, 1e-20));
    return !(any(isnan(radiance)) || any(isinf(radiance)));
}

#endif
