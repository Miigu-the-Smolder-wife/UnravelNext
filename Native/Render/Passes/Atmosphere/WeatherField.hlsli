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
#endif
