// Shared ordered air-slice estimator; the queue only changes which lane owns a slice.
#ifndef UNX_FROXEL_SLICE_HLSLI
#define UNX_FROXEL_SLICE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"
#include "Passes/Shadow/VsmLocalAirWalk.hlsli"
#include "Passes/Lights/LightFunction.hlsli"

float3 froxelSelfAttenuation(float3 x) { return select(x > 1e-4, (1 - exp(-x)) / max(x, 1e-4), 1 - 0.5 * x); }

static const float kGaussX[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                  0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363 };
static const float kGaussW[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                  0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763 };

// Local lights' air walk statistics (P[2].w bit 16, measurement only) and its cap.
struct AirLocalCount
{
    uint entries, cells, maxCells, runs, capped;
};

// Angle map of light l over the segment o + dir t, t in [0, len]: t = tc + h tan theta, theta in [th0, th1] = mid +- half.
struct AirLocalMap
{
    float tc, h, th0, th1, mid, half;
};
AirLocalMap airLocalMap(GpuLight l, float3 o, float3 dir, float len)
{
    AirLocalMap mp;
    mp.tc = dot(l.position - o, dir);
    // Distance of the line from the light, not below the emitter's size (1 cm for points): the point-source integrand
    // is singular on the line.
    mp.h = max(length(l.position - (o + dir * mp.tc)), max(max(l.size.x, l.size.y), 0.01));
    mp.th0 = atan(-mp.tc / mp.h);
    mp.th1 = atan((len - mp.tc) / mp.h);
    mp.mid = 0.5 * (mp.th0 + mp.th1);
    mp.half = 0.5 * (mp.th1 - mp.th0);
    return mp;
}

// The 8-node Gauss-Legendre rule of light l's air in-scattering over the segment (nits); partial: over the lit set of
// Legendre moments m (VsmLocalAirWalk.hlsli), else over the whole segment.
float3 airLocalEval(GpuLight l, float3 o, float3 dir, AirLocalMap mp, AirCoefficients c, float mieG, uint functions, uint lightIndex, float lateral,
                    bool partial, float m[8])
{
    const bool withFunction = functions != LIGHT_FUNCTION_NONE && (lightType(l) == LIGHT_POINT || lightType(l) == LIGHT_SPOT);
    float3 sum = 0, full = 0;
    [loop] for (uint i = 0; i < 8; ++i)
    {
        const float th = mp.mid + mp.half * kGaussX[i];
        const float t = mp.tc + mp.h * tan(th);
        const float3 v = o + dir * t - l.position;
        const float d = mp.h / cos(th);
        const float3 w = v / max(length(v), 1e-6);
        const float nu = -sin(th);  // cosine between the light's propagation (w) and the path to the camera (-dir)
        const float3 phase = c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, mieG);
        const float3 f = withFunction ? lightFunction(functions, lightIndex, l.forward, l.right, w, lateral / max(d, 1e-4), g_time) : 1.0;
        const float3 value = froxelIntensity(l, w) * froxelWindow(l, d) * f * phase * exp(-c.extinction * max(t, 0.0));
        sum += (partial ? vsmLocalAirWeight(kGaussX[i], kGaussW[i], m) : kGaussW[i]) * value;
        full += kGaussW[i] * value;
    }
    // Partly lit: the interpolant over the lit set, within [0, the whole segment's] (the integrand is not negative).
    if (partial) sum = clamp(sum, 0.0, full);
    return sum * (mp.half / mp.h) * l.color;
}

// Air in-scattering of an unshadowed light along o + dir t, t in [0, len] (nits), relative to the segment's start.
// Shadowed lights are walked in pieces by the group (main).
float3 airLocalLight(GpuLight l, float3 o, float3 dir, float len, AirCoefficients c, float mieG, uint functions, uint lightIndex, float lateral)
{
    float m[8] = (float[8])0;
    return airLocalEval(l, o, dir, airLocalMap(l, o, dir, len), c, mieG, functions, lightIndex, lateral, false, m);
}

// Parameter of piece boundary q of K pieces uniform in theta (q = 0: 0, q = K: len). Both pieces at a boundary evaluate
// this same expression, so the pieces tile [0, len] with no gap or overlap.
float airLocalPieceT(AirLocalMap mp, float len, uint q, uint K)
{
    if (q == 0) return 0;
    if (q >= K) return len;
    return clamp(mp.tc + mp.h * tan(mp.th0 + (mp.th1 - mp.th0) * ((float)q / (float)K)), 0.0, len);
}


// Shadow slot of list position pos's light (VSM_LOCAL_NONE: unshadowed, or no slot this frame).
uint airLocalSlot(ByteAddressBuffer lists, FroxelGrid g, uint pos, bool localShadows, out uint li)
{
    const uint w = lists.Load(g.indexBase + (pos >> 1) * 4);
    const uint entry = (pos & 1) ? w >> 16 : w & 0xFFFFu;
    li = entry & 0x7FFFu;
    if (!localShadows || (entry & 0x8000u) == 0) return VSM_LOCAL_NONE;
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[3].y];
    return slotOf[li];
}

struct FroxelAirResult
{
    float3 tau, source, sky;
    VsmAirWalkCount walk;
};
FroxelAirResult froxelAirSlice(FroxelGrid g, uint2 tile, uint s)
{
    const AtmosphereParams a = airParamsFromTexels(P[0].z);
    const uint tlut = P[0].z, mlut = P[0].w;
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float3 sun = normalize(g_sunDirection);
    const float3 E = g_sunIlluminance * g_sunColor;
    const float nu = dot(dir, sun);
    const float phaseR = airRayleighPhase(nu), phaseM = airMiePhase(nu, a.mieG);
    const float stepAltitude = asfloat(P[2].z);
    const uint experiment = P[2].w;
    const float tStart = airViewStart(g_clipPlane, g_cameraPosition, dir);
    const float zs0 = froxelNodeDepth(g, s), zs1 = froxelNodeDepth(g, s + 1);
    float3 tau = 0, source = 0, skyTerm = 0;
    VsmAirWalkCount walk = (VsmAirWalkCount)0;
    const float z0 = zs0, z1 = zs1;
    const float t0 = max(z0 * toRay, tStart), len = z1 * toRay - t0;
    const float3 o = g_cameraPosition + dir * t0;
    const float nearScale = airNearScale(t0, len);  // (AtmosphereCommon.hlsli: no atmosphere before AIR_VIEW_START_M)
    // Substeps: the air's density is exponential in altitude; midpoint steps of at most stepAltitude.
    const float h0 = airAltitude(a, o), h1 = airAltitude(a, o + dir * len), hm = airAltitude(a, o + dir * (0.5 * len));
    const float dh = max(max(abs(h1 - h0), abs(hm - h0)), abs(hm - h1));
    const uint steps = (experiment & 4) ? 0u : (uint)clamp(ceil(dh / stepAltitude), 1.0, 32.0);
    const float dt = len / steps;
    float3 single = 0, multi = 0;
    [loop] for (uint k = 0; k < steps; ++k)
    {
        const float3 p = airLiftToSurface(a, o + dir * ((k + 0.5) * dt));
        const AirCoefficients c = airScaled(airCoefficients(a, max(0.0, airAltitude(a, p))), nearScale);
        const float3 w = exp(-tau) * airIntegral(c.extinction, dt);
        single += w * (c.rayleigh * phaseR + c.mie * phaseM) * ((experiment & 8) ? 1.0 : airSunTransmittance(a, tlut, p, sun));
        multi += w * (c.rayleigh + c.mie) * ((experiment & 16) ? 1.0 : airMultipleScattering(a, mlut, p, dir, sun));
        tau += c.extinction * dt;
    }
    // Casters' shadows in the air: the shadowed fraction of the segment removes that part of the single scattering.
    float f = 0;
    if (P[1].w != 0xFFFFFFFFu && any(single > 0) && (experiment & 1) == 0)
    {
        VsmResources r;
        r.table = ResourceDescriptorHeap[P[1].x];
        r.pool = ResourceDescriptorHeap[P[1].y];
        r.blocks = ResourceDescriptorHeap[P[1].z];
        r.searchBound = ResourceDescriptorHeap[P[2].x];
        r.cbv = P[1].w;
        ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[1].w];
        uint k;
        if (vsmAirLevel(vc, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[2].y), k))
        {
            f = vsmAirShadowFraction(r, o, o + dir * len, k, walk, (experiment & 32) != 0);
            ++walk.slices;
        }
    }
    source = E * (single * (1 - f) + multi);
    skyTerm = -E * single * f;  // what the sky LUT has and the shadows remove
    FroxelAirResult result;
    result.tau = tau; result.source = source; result.sky = skyTerm; result.walk = walk;
    return result;
}
// Exactly 64 bytes per original froxel index, no reduced precision or atomics.
void froxelStoreAir(RWByteAddressBuffer b, uint index, FroxelAirResult v)
{
    const uint at = index * 64;
    b.Store4(at, uint4(asuint(v.tau), v.walk.slices));
    b.Store4(at + 16, uint4(asuint(v.source), v.walk.mixedPages));
    b.Store4(at + 32, uint4(asuint(v.sky), v.walk.blocks32));
    b.Store4(at + 48, uint4(v.walk.blocks8, v.walk.texels, v.walk.capped, 0));
}
FroxelAirResult froxelLoadAir(ByteAddressBuffer b, uint index)
{
    const uint at = index * 64;
    const uint4 a = b.Load4(at), c = b.Load4(at + 16), d = b.Load4(at + 32), e = b.Load4(at + 48);
    FroxelAirResult v;
    v.tau = asfloat(a.xyz); v.source = asfloat(c.xyz); v.sky = asfloat(d.xyz);
    v.walk.slices = a.w; v.walk.mixedPages = c.w; v.walk.blocks32 = d.w;
    v.walk.blocks8 = e.x; v.walk.texels = e.y; v.walk.capped = e.z;
    return v;
}
#endif
