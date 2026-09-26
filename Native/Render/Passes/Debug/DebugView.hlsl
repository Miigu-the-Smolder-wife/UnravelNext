// unx-kernel: cs_6_6 main
// Buffer visualization (E, A15; FEATURES_GAME 7.1, quality key debug.view): one of the view's buffers colour-mapped into
// the view's colour instead of the frame (a separate pass, the tone curve bypassed). Each mode maps its buffer to a
// display-encoded value e in [0, 1] (what an SDR output holds); linear outputs get e decoded (paper white 1).
// Radiance buffers are shown as linearToSrgb(x / (1 + x)) of radiance x exposure (Reinhard: every value visible).
// At the probe pixel (debug.view_probe) the raw values are printed next to a cross (DebugDraw.hlsli).
// P[0] = { mode, source SRV, second SRV (visible clusters / none), colour UAV }, P[1] = { linear output, index (hiz mip
// or froxel slice), probe x, probe y (0xFFFFFFFF = none) }, P[2] = { froxel tile px, froxel slices, 0, 0 },
// P[3] = { view width, view height, 0, 0 }; frame constants b1 = the view.
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "VisBuffer.hlsli"
#include "Passes/Debug/DebugDraw.hlsli"

#define VIEW_VIS_ID 0u
#define VIEW_INSTANCE 1u
#define VIEW_DEPTH 2u
#define VIEW_HIZ 3u
#define VIEW_NORMAL 4u
#define VIEW_ALBEDO 5u
#define VIEW_ROUGHNESS 6u
#define VIEW_SHADOW_SUN 7u
#define VIEW_REFLECTION 8u
#define VIEW_PARTICLE_LAYER 9u
#define VIEW_DISTORTION 10u
#define VIEW_VOLUME_TAU 11u
#define VIEW_VOLUME_SOURCE 12u
#define VIEW_AIR_TRANSMITTANCE 13u
#define VIEW_COVERAGE 14u

uint debugHash(uint x)  // PCG (O'Neill), one round
{
    const uint state = x * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float3 debugHashColor(uint x)
{
    const uint h = debugHash(x);
    return float3(h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu) / 255.0f * 0.75f + 0.25f;
}
// Turbo colormap, polynomial fit (A. Mikhailov's Turbo; fit by R. Du, Apache 2.0).
float3 debugTurbo(float t)
{
    t = saturate(t);
    const float4 v4 = float4(1, t, t * t, t * t * t);
    const float2 v2 = v4.zw * v4.z;
    return saturate(float3(dot(v4, float4(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f)) + dot(v2, float2(-152.94239396f, 59.28637943f)),
                           dot(v4, float4(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f)) + dot(v2, float2(4.27729857f, 2.82956604f)),
                           dot(v4, float4(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f)) + dot(v2, float2(-89.90310912f, 27.34824973f))));
}
// View depth (m) of a reversed-Z device depth, shown as Turbo over log depth from the near plane to 10 km; sky black.
float3 debugDepthColor(float z, out float depthM)
{
    depthM = z > 0 ? g_nearPlane / z : asfloat(0x7F800000u);
    if (!(z > 0)) return 0;
    return debugTurbo(log2(depthM / g_nearPlane) / log2(10000.0f / g_nearPlane));
}
float3 debugRadiance(float3 x)
{
    const float3 r = max(x, 0) / (1 + max(x, 0));
    return float3(linearToSrgb(r.r), linearToSrgb(r.g), linearToSrgb(r.b));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy;
    if (any(id.xy >= size)) return;
    const uint mode = P[0].x;
    float3 e = 0;     // display-encoded colour
    float4 raw = 0;   // probe values
    uint rawCount = 1;
    switch (mode)
    {
    case VIEW_VIS_ID:
    case VIEW_INSTANCE:
    {
        Texture2D<uint> vis = ResourceDescriptorHeap[P[0].y];
        const uint v = vis.Load(int3(id.xy, 0));
        if (v == VIS_NONE) break;
        if (mode == VIEW_VIS_ID)
        {
            e = debugHashColor(visVisibleCluster(v)) * (0.8f + 0.2f * ((debugHash(visTriangle(v)) & 0xFFu) / 255.0f));
            raw = float4(v, visVisibleCluster(v), visTriangle(v), 0);
            rawCount = 3;
        }
        else
        {
            StructuredBuffer<GpuVisibleCluster> list = ResourceDescriptorHeap[P[0].z];
            const uint instance = list[visVisibleCluster(v)].instance;
            e = debugHashColor(instance);
            raw = float4(instance, 0, 0, 0);
        }
        break;
    }
    case VIEW_DEPTH:
    case VIEW_HIZ:
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
        const uint mip = mode == VIEW_HIZ ? P[1].y : 0u;
        const uint2 texel = mode == VIEW_HIZ ? (id.xy >> (mip + 1)) : id.xy;
        float m;
        e = debugDepthColor(depth.Load(int3(texel, mip)), m);
        raw = float4(m, 0, 0, 0);
        break;
    }
    case VIEW_NORMAL:
    case VIEW_ALBEDO:
    case VIEW_ROUGHNESS:
    {
        Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
        const uint2 packed = gbuffer.Load(int3(id.xy, 0));
        const GBufferSample s = decodeGBuffer(packed);
        if (mode == VIEW_NORMAL) { e = s.normal * 0.5f + 0.5f; raw = float4(s.normal, 0); rawCount = 3; }
        else if (mode == VIEW_ALBEDO)
        {
            e = float3(packed.y & 0xFFu, (packed.y >> 8) & 0xFFu, (packed.y >> 16) & 0xFFu) / 255.0f;  // stored sRGB8
            raw = float4(s.baseColor, 0);
            rawCount = 3;
        }
        else { e = s.roughness; raw = float4(s.roughness, 0, 0, 0); }
        break;
    }
    case VIEW_SHADOW_SUN:
    {
        Texture2D<uint> visibility = ResourceDescriptorHeap[P[0].y];
        const float v = (visibility.Load(int3(id.xy, 0)) & 0xFFu) / 255.0f;
        e = v;
        raw = float4(v, 0, 0, 0);
        break;
    }
    case VIEW_REFLECTION:
    {
        Texture2D<float4> reflection = ResourceDescriptorHeap[P[0].y];
        const float4 r = reflection.Load(int3(id.xy, 0));  // nit
        e = debugRadiance(r.rgb * g_exposure);
        raw = r;
        rawCount = 4;
        break;
    }
    case VIEW_PARTICLE_LAYER:
    {
        Texture2D<float4> layer = ResourceDescriptorHeap[P[0].y];
        const float4 l = layer.Load(int3(id.xy / 4, 0));  // x exposure already; a = transmittance
        e = debugRadiance(l.rgb);
        raw = l;
        rawCount = 4;
        break;
    }
    case VIEW_DISTORTION:
    {
        Texture2D<float2> offset = ResourceDescriptorHeap[P[0].y];
        const float2 d = offset.Load(int3(id.xy / 4, 0));  // px
        e = float3(saturate(0.5f + d / 16.0f), 0.5f);
        raw = float4(d, 0, 0);
        rawCount = 2;
        break;
    }
    case VIEW_VOLUME_TAU:
    case VIEW_VOLUME_SOURCE:
    case VIEW_AIR_TRANSMITTANCE:
    {
        Texture3D<float4> froxels = ResourceDescriptorHeap[P[0].y];
        const uint tile = P[2].x, slices = P[2].y, s = min(P[1].y, slices - 1);
        // volumeSlices: part 0 tau, part 1 source (nit); air volume: parts of S + 1 nodes, part 1 optical depth
        const uint z = mode == VIEW_VOLUME_TAU ? s : (mode == VIEW_VOLUME_SOURCE ? slices + s : (slices + 1) + s);
        const float3 v = froxels.Load(int4(id.xy / tile, z, 0)).rgb;
        e = mode == VIEW_VOLUME_SOURCE ? debugRadiance(v * g_exposure) : (mode == VIEW_VOLUME_TAU ? 1 - exp(-v) : exp(-v));
        raw = float4(v, 0);
        rawCount = 3;
        break;
    }
    case VIEW_COVERAGE:
    {
        Texture2D<uint2> range = ResourceDescriptorHeap[P[0].y];
        const uint2 r = range.Load(int3(id.xy, 0));
        const bool records = !(r.x == 0 && r.y == 0xFFFFFFFFu);
        e = records ? float3(1, 0.8f, 0.2f) : 0.05f;
        raw = float4(records ? 1 : 0, 0, 0, 0);
        break;
    }
    default: e = float3(1, 0, 1); break;
    }
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].w];
    float3 outColour = saturate(e);
    if (P[1].x != 0) outColour = float3(srgbToLinear(outColour.r), srgbToLinear(outColour.g), srgbToLinear(outColour.b));
    colour[id.xy] = float4(outColour, 1);

    if (id.x == P[1].z && id.y == P[1].w)
    {
        const float3 p = float3(id.xy + 0.5f, 0);
        const float4 cross = float4(1, 1, 0, 1);
        debugLine(p - float3(8, 0, 0), p - float3(3, 0, 0), cross, 1, DEBUG_SCREEN);
        debugLine(p + float3(3, 0, 0), p + float3(8, 0, 0), cross, 1, DEBUG_SCREEN);
        debugLine(p - float3(0, 8, 0), p - float3(0, 3, 0), cross, 1, DEBUG_SCREEN);
        debugLine(p + float3(0, 3, 0), p + float3(0, 8, 0), cross, 1, DEBUG_SCREEN);
        for (uint k = 0; k < rawCount; ++k) debugNumber(p, float2(12, 4 + 16 * k), raw[k], 16, float4(1, 1, 1, 1), DEBUG_SCREEN | DEBUG_SHADOW);
    }
}
