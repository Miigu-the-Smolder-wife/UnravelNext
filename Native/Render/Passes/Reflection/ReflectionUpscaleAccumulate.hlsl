// unx-kernel: cs_6_6 main
// unx-strict-fp
// Output-grid reflection history. Exactly nine current samples and four previous
// history taps per output pixel; no atomic writes or data-dependent walks.
// Root words match accumulateOutputReflection in ReflectionSystem.cpp.
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

float4 unjitteredClip(float4 p)
{
    float4 c = mul(g_viewProj, p);
    c.xy -= float2(2 * asfloat(P[5].z), -2 * asfloat(P[5].w)) / float2(P[3].xy) * c.w;
    return c;
}
uint sampleInstance(Texture2D<uint> vis, uint2 p)
{
    const uint v = vis.Load(int3(p, 0));
    return v == VIS_NONE ? 0 : loadVisibleCluster(P[1].y, visVisibleCluster(v)).instance + 1;
}
bool rayMode(Texture2D<float4> reflection, Texture2D<uint> modes, uint2 p)
{
    if (reflection.Load(int3(p.x / 8, P[3].y + p.y / 8, 0)).a < 0.5) return false;
    const uint mode = reflMode(modes.Load(int3(p, 0)));
    return mode == REFL_M || mode == REFL_G;
}
[numthreads(8, 8, 1)]
void main(uint2 o : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy, outputSize = P[5].xy;
    if (any(o >= outputSize)) return;
    Texture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> control = ResourceDescriptorHeap[P[10].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    Texture2D<float2> hit = ResourceDescriptorHeap[P[1].z];
    RWTexture2D<float4> result = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint2> resultKeys = ResourceDescriptorHeap[P[2].z];
    const float2 uv = (float2(o) + 0.5) / float2(outputSize), jitter = asfloat(P[5].zw);
    const float2 position = uv * float2(size);
    const int2 centre = clamp(int2(floor(position + jitter)), 0, int2(size) - 1);
    const uint key = sampleInstance(vis, centre);
    if (key == 0 || !rayMode(reflection, modes, centre))
    {
        result[o] = 0; resultKeys[o] = 0; return;
    }
    const ReflSurface surface = reflSurface(depth, gbuffer, centre);
    const float nv = max(abs(dot(surface.normal, surface.view)), 0.1);
    const float tolerance = surface.linearDepth * (1e-3 + 2 * asfloat(P[4].w) / nv);
    const float2 scale = float2(outputSize) / float2(size);
    // A subpixel glossy-radiance footprint, in output pixels. The old
    // internal history broadened this footprint again on every reprojection.
    const float K = 4;
    const float perFrame = 3.14159265 / (K * scale.x * scale.y);
    float3 sum = 0, wide = 0;
    float weight = 0, wideWeight = 0, motion = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 p = clamp(centre + int2(x, y), 0, int2(size) - 1);
            if (sampleInstance(vis, p) != key || !rayMode(reflection, modes, p)) continue;
            const float4 value = reflection.Load(int3(p, 0));
            const float3 world = worldFromDepth(float2(p), depth.Load(int3(p, 0)));
            if (value.a < 0.5 || any(!isfinite(value)) || abs(dot(surface.normal, world - surface.position)) > tolerance) continue;
            const float2 d = float2(p) + 0.5 - jitter - position;
            const float w = exp(-K * dot(d * scale, d * scale));
            const float ww = exp(-2.29 * dot(d, d));
            sum += w * (value.rgb - control.Load(int3(p, 0)).rgb); weight += w;
            wide += ww * (value.rgb - control.Load(int3(p, 0)).rgb); wideWeight += ww;
            motion = max(motion, hit.Load(int3(p, 0)).y);
        }
    if (wideWeight <= 0) { result[o] = 0; resultKeys[o] = 0; return; }
    wide /= wideWeight;
    float3 previousP, previousN;
    uint previousInstance;
    giPreviousSurface(P[1].x, P[1].y, centre, surface.position, surface.normal, previousP, previousN, previousInstance);
    const float4 worldPoint = float4(surface.position, 1), displacement = float4(previousP - surface.position, 0);
    const float4 clip = unjitteredClip(worldPoint);
    const float4x4 deltaVP = float4x4(asfloat(P[6]), asfloat(P[7]), asfloat(P[8]), asfloat(P[9]));
    const float4 delta = mul(deltaVP, worldPoint + displacement) + unjitteredClip(displacement);
    const float previousW = clip.w + delta.w;
    const float2 motionUv = (delta.xy - clip.xy / clip.w * delta.w) / max(previousW, 1e-6) * float2(-0.5, 0.5);
    const float2 prevUv = uv - motionUv;
    float3 previous = 0;
    float n = 0, accepted = 0, minimumN = 3e38;
    if ((P[3].w & 3u) == 0 && previousW > 0 && all(prevUv >= 0) && all(prevUv <= 1))
    {
        Texture2D<float4> before = ResourceDescriptorHeap[P[1].w];
        Texture2D<uint2> keys = ResourceDescriptorHeap[P[2].x];
        const float2 previousPixel = prevUv * float2(outputSize) - 0.5;
        // Exact zero motion keeps the texel exactly, including at non-power-of-two sizes.
        const int2 first = all(motionUv == 0) ? int2(o) : int2(floor(previousPixel));
        const float2 fraction = all(motionUv == 0) ? 0 : frac(previousPixel);
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const int2 d = int2(i & 1, i >> 1), p = first + d;
            if (any(p < 0) || any(p >= int2(outputSize))) continue;
            const float w = (d.x ? fraction.x : 1 - fraction.x) * (d.y ? fraction.y : 1 - fraction.y);
            if (w <= 0) continue;
            const uint2 stored = keys.Load(int3(p, 0));
            const float4 value = before.Load(int3(p, 0));
            if (stored.x != key || abs(asfloat(stored.y) - previousW) > tolerance || any(!isfinite(value))) continue;
            previous += value.rgb * w; accepted += w; minimumN = min(minimumN, value.a);
        }
        if (accepted > 0 && minimumN > 0)
        {
            previous /= accepted;
            const float3 r = reflect(-surface.view, surface.normal);
            const float3 prevView = normalize(asfloat(P[4].xyz) - previousP);
            const float3 prevR = reflect(-prevView, previousN);
            const float travel = 2 * asin(saturate(0.5 * length(r - prevR)));
            const float lobe = reflectionLobeHalfAngle(surface.roughness, max(dot(surface.normal, surface.view), 1e-4));
            const float viewWindow = travel > 0 ? asfloat(P[3].z) * lobe / travel : 1e9;
            const float hitWindow = motion > 0 ? asfloat(P[3].z) / motion : 1e9;
            n = min(minimumN, min((float)P[2].w, floor(min(viewWindow, hitWindow))) * perFrame);
        }
    }
    const float3 current = weight > 1e-5 ? sum / weight : wide;
    const float3 value = n > 0 ? lerp(previous, current, weight / max(weight + n, 1e-9)) : wide;
    result[o] = float4(value, min(n + weight, P[2].w * perFrame));
    resultKeys[o] = uint2(key, asuint(surface.linearDepth));
}
