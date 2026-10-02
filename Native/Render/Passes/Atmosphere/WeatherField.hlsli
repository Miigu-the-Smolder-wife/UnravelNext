// The frame's weather for GPU consumers (B6; Docs/Design/Requests/20260926_B_weather_fields.md 2-3): the World's
// weather row (FrameContext::weather) and S's rain shadow map, in one raw record (FrameResources::weather, 96 B).
//   rainExposure(srv, x)  0..1: how much of the rain reaches x -- the map holds, per texel of a plane normal to the rain
//                         direction (512^2, 128 m around the camera), the distance along the rain to the first surface
//                         it meets (one ray per texel through the ray scene, every frame it rains); a point is exposed
//                         when it lies no deeper than that surface's depth interpolated at its place (+ 0.05 m): exact on
//                         planar receivers at any tilt, edges to one texel (0.25 m). Outside the map: 1 (open). Declare
//                         FrameResources::rainShadow to read it.
//   weatherValues(srv)    rain rate (mm/h), wetness, snow rate (mm/h), snow depth (m), fog density, cloud cover.
// Record: { rain, wetness, snow rate, snow depth, fog, cloud cover, 0, 0, rain direction xyz, map SRV, map origin xyz
// (texel (0, 0) corner on the top plane), cell m, axis u xyz, texels per side, axis v xyz, 0 }.
// Shores (from byte 96: the count, then from byte 112 48 B per water body - the frame's basins and its sea, the nearest
// WEATHER_SHORES of them; AtmosphereSystem.cpp publishWeather):
//   { centre x, still level y, centre z, band (m above the level that the water wets: its ripples and splashes) },
//   { half size x, half size z (a round body: its radius in x), cos, sin of the body's yaw },
//   { shape (0 rectangle, 1 circle, 2 without bounds: the open sea), margin (m beyond the outline over which the wetness
//     ends), depth (m of water; 0: no floor), 0 }.
//   shoreWetness(srv, x, under)  how wet the water bodies leave a surface point: 1 under a still level (under = 1: the
//                         point is in the water), falling to 0 over the band above it and over the margin beyond the
//                         outline; nothing under a body's floor (the room below a bath).
#ifndef UNX_ATMOSPHERE_WEATHER_FIELD_HLSLI
#define UNX_ATMOSPHERE_WEATHER_FIELD_HLSLI
#include "Bindless.hlsli"

struct WeatherRecord
{
    float rainRate, wetness, snowRate, snowDepth, fogDensity, cloudCover;
    float3 rainDirection;
    uint mapSrv;
    float3 mapOrigin;
    float cell;
    float3 axisU;
    uint texels;
    float3 axisV;
};

WeatherRecord weatherLoad(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    WeatherRecord w;
    const float4 a = asfloat(b.Load4(0)), c = asfloat(b.Load4(16));
    w.rainRate = a.x;
    w.wetness = a.y;
    w.snowRate = a.z;
    w.snowDepth = a.w;
    w.fogDensity = c.x;
    w.cloudCover = c.y;
    const uint4 d = b.Load4(32), e = b.Load4(48), f = b.Load4(64), g = b.Load4(80);
    w.rainDirection = asfloat(d.xyz);
    w.mapSrv = d.w;
    w.mapOrigin = asfloat(e.xyz);
    w.cell = asfloat(e.w);
    w.axisU = asfloat(f.xyz);
    w.texels = f.w;
    w.axisV = asfloat(g.xyz);
    return w;
}

float rainExposure(uint srv, float3 x)
{
    if (srv == 0xFFFFFFFFu) return 1;
    const WeatherRecord w = weatherLoad(srv);
    if (w.mapSrv == 0xFFFFFFFFu) return 1;
    const float3 rel = x - w.mapOrigin;
    const float2 st = float2(dot(rel, w.axisU), dot(rel, w.axisV)) / w.cell - 0.5;  // texel-centre coordinates
    if (any(st < -0.5) || any(st > w.texels - 0.5)) return 1;
    const float depth = dot(rel, w.rainDirection);
    Texture2D<float> map = ResourceDescriptorHeap[w.mapSrv];
    const int2 i0 = int2(floor(st));
    const float2 f = st - floor(st);
    // The first surface's depth interpolated at x's place, then one comparison: exact on a planar receiver whatever the
    // rain's tilt (comparing with each texel's own depth failed on a flat roof under slanted rain: the depths of the texels
    // differ by cell x tan(tilt) across it). A surface's edge is resolved to one texel.
    float surface = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 t = clamp(i0 + o, 0, int(w.texels) - 1);
        surface += (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * map.Load(int3(t, 0));
    }
    return depth <= surface + 0.05 ? 1.0 : 0.0;
}

#define WEATHER_SHORES 8u
float shoreWetness(uint srv, float3 x, out float under)
{
    under = 0;
    if (srv == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    const uint count = min(b.Load(96), WEATHER_SHORES);
    float wet = 0;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint at = 112 + 48 * i;
        const float4 r0 = asfloat(b.Load4(at)), r1 = asfloat(b.Load4(at + 16)), r2 = asfloat(b.Load4(at + 32));
        const float h = x.y - r0.y;  // above the still level
        if (h >= r0.w || (r2.z > 0 && h < -(r2.z + r2.y))) continue;
        float outside = -1;  // m outside the body's outline
        if (r2.x < 1.5)
        {
            // the body's axes (Pool.cpp: local x = dx cos - dz sin, local z = dx sin + dz cos)
            const float2 d = x.xz - r0.xz;
            const float2 l = float2(d.x * r1.z - d.y * r1.w, d.x * r1.w + d.y * r1.z);
            outside = r2.x < 0.5 ? max(abs(l.x) - r1.x, abs(l.y) - r1.y) : length(l) - r1.x;
        }
        if (outside >= r2.y) continue;
        const float across = saturate(1.0 - max(outside, 0.0) / max(r2.y, 1e-3));
        wet = max(wet, across * (h <= 0 ? 1.0 : saturate(1.0 - h / max(r0.w, 1e-3))));
        if (h <= 0 && outside <= 0) under = 1;
    }
    return wet;
}
#endif
