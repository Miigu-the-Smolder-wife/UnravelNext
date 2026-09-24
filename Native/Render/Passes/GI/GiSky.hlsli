// Sky, sun and random helpers shared by R's ray libraries (GI cache update, reflections). The including library is
// compiled in two variants (// unx-variants: SKY=0,1):
//   SKY0 (SKY_ATMOSPHERE): escaping rays see S's sky (Atmosphere.hlsli), the sun's illuminance includes transmittance;
//   SKY1 (SKY_CONSTANT): constant sky radiance and sun illuminance from root constants (tests, builds without S).
// Root constants used here: P[1].xyz = constant sky radiance (SKY1), P[1].w = ray length, P[2] = atmosphere SRVs
// {transmittance, multiScatter, skyView, aerial} (SKY0; UNX_NONE = no sky), P[3].xyz = constant sun illuminance (SKY1).
#ifndef UNX_GI_SKY_HLSLI
#define UNX_GI_SKY_HLSLI
#define SKY_ATMOSPHERE 0
#define SKY_CONSTANT 1
#include "Bindless.hlsli"
#include "Frame.hlsli"
#if SKY == SKY_ATMOSPHERE
#include "Passes/Atmosphere/Atmosphere.hlsli"
#endif

uint giRandom(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float giUnit(uint x) { return (giRandom(x) >> 8) * (1.0 / 16777216.0); }

float giRayLength() { return asfloat(P[1].w); }

float3 giSkyRadiance(float3 dir)
{
#if SKY == SKY_ATMOSPHERE
    AtmosphereSrvs a = { P[2].x, P[2].y, P[2].z, P[2].w };
    return P[2].z == UNX_NONE ? 0 : atmosphereSkyRadiance(a, dir);
#else
    return asfloat(P[1].xyz);
#endif
}

// Direct solar illuminance on a surface facing the sun at p (without shadowing).
float3 giSunIlluminance(float3 p)
{
#if SKY == SKY_ATMOSPHERE
    AtmosphereSrvs a = { P[2].x, P[2].y, P[2].z, P[2].w };
    return P[2].x == UNX_NONE ? 0 : atmosphereSunIlluminance(a, p);
#else
    return asfloat(P[3].xyz);
#endif
}

// Uniform direction within the solar disk.
float3 giSunDirection(uint seed)
{
    const float3 l = normalize(g_sunDirection);
    const float s = l.z >= 0 ? 1.0 : -1.0;  // Duff et al. 2017 basis
    const float a = -1.0 / (s + l.z);
    const float c = l.x * l.y * a;
    const float3 t = float3(1 + s * l.x * l.x * a, s * c, -s * l.x);
    const float3 b = float3(c, s + l.y * l.y * a, -l.y);
    const float r = tan(g_sunAngularRadius) * sqrt(giUnit(seed));
    const float phi = 6.28318530718 * giUnit(seed ^ 0x9e3779b9u);
    return normalize(l + (t * cos(phi) + b * sin(phi)) * r);
}

#endif
