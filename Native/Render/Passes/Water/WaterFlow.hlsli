// Flow on a basin's surface (W; FEATURES_GAME 1.3 (g): streams, channels and drains - an appearance model, not a
// simulation; the bath-house's water channels): the surface's small waves drift with a velocity field.
//   field   v = a uniform stream along the basin + a velocity map over it (RG = v / the map's speed, 0.5 = still) + a
//           drain (a sink of strength Q with a circulation Gamma about a point: the speed Q / 2 pi r inward and Gamma /
//           2 pi r around, both held at their values 3 cm from the point: the core);
//   waves   six sine waves of wavelengths waveLength .. waveLength / 3, their directions spread by the golden angle - the
//           slope of a height field (analytic: no texture), of rms slope waveSlope where the water moves at 0.2 m/s or
//           more and less in slower water (still water has none of them);
//   drift   the flow map's two phases: phase_i = frac(t / T + i / 2); the waves are read at p - v (phase_i - 1/2) T (each
//           phase carries the pattern along v for a period, then jumps back) and blended with the triangle weights
//           1 - |2 phase_i - 1| (a phase weighs nothing when it jumps), the sum renormalised by the weights' root sum
//           of squares (the two patterns do not correlate: the plain blend loses a third of its slope at mid-period).
//           T is the basin's: the time its fastest water takes over two wavelengths, held to 1 .. 12 s;
//   funnel  the free surface of the drain's vortex, h = -Gamma^2 / (8 pi^2 g r^2): its slope, held at 1;
//   filter  a pixel does not resolve a wave shorter than twice its footprint: each wave's slope fades between four and
//           two footprints per wavelength, and what fades is returned as slope variance for the lobes (the sun's lobe,
//           the mirror cone: WaterSurface.hlsli) - a far channel glitters instead of aliasing.
// The slot table's flow block (WaterSurface.cpp: after the sea's block), 80 B per stream slot:
//   +0   centre x, z; cos, sin of the basin's yaw (Pool.cpp: local x = dx cos - dz sin, local z = dx sin + dz cos)
//   +16  the stream's velocity x, z (local, m/s); the drain's place x, z (local, m from the centre)
//   +32  the drain's inflow Q and circulation Gamma (m^2/s); wave length (m; 0: the stream has no flow); wave rms slope
//   +48  velocity map SRV (UNX_NONE: none), its speed (m/s at full scale), the period T (s), frac(t / T) of the frame's
//        time (taken in double on the CPU: a float of the World's seconds loses the phase after hours)
//   +64  size x, size z (m: the map's extent over the basin), 0, 0
#ifndef UNX_WATER_FLOW_HLSLI
#define UNX_WATER_FLOW_HLSLI
#include "Bindless.hlsli"

#define WATER_FLOW_BYTES 80u
#define WATER_FLOW_WAVES 6u

struct WaterFlow
{
    float2 centre;
    float cosYaw, sinYaw;
    float2 velocity, drain;
    float inflow, circulation, waveLength, waveSlope;
    uint map;
    float mapSpeed, period, cycle;
    float2 size;
};

// false: the stream has no flow.
bool waterFlowLoad(uint table, uint at, out WaterFlow f)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    const float4 r2 = asfloat(b.Load4(at + 32));
    f = (WaterFlow)0;
    if (!(r2.z > 0)) return false;
    const float4 r0 = asfloat(b.Load4(at)), r1 = asfloat(b.Load4(at + 16));
    const uint4 r3 = b.Load4(at + 48);
    f.centre = r0.xy;
    f.cosYaw = r0.z, f.sinYaw = r0.w;
    f.velocity = r1.xy, f.drain = r1.zw;
    f.inflow = r2.x, f.circulation = r2.y, f.waveLength = r2.z, f.waveSlope = r2.w;
    f.map = r3.x;
    f.mapSpeed = asfloat(r3.y);
    f.period = max(asfloat(r3.z), 1e-3);
    f.cycle = asfloat(r3.w);
    f.size = asfloat(b.Load2(at + 64));
    return true;
}

// The velocity at a local point (basin axes, m from the centre; m/s).
float2 waterFlowVelocity(WaterFlow f, float2 l)
{
    float2 v = f.velocity;
    if (f.map != UNX_NONE)
    {
        Texture2D<float4> map = ResourceDescriptorHeap[f.map];
        v += (map.SampleLevel(g_linearClamp, l / max(f.size, 1e-3) + 0.5, 0).rg * 2.0 - 1.0) * f.mapSpeed;
    }
    if (f.inflow != 0 || f.circulation != 0)
    {
        const float2 d = l - f.drain;
        const float distance = length(d);
        const float2 radial = d / max(distance, 1e-6);
        v += (f.circulation * float2(-radial.y, radial.x) - f.inflow * radial) / (6.2831853 * max(distance, 0.03));
    }
    return v;
}

// The wave field's slope at p (m) for unit rms slope, low-passed to the footprint (m); lost: the share of the slope
// variance the footprint removed, [0, 1].
float2 waterFlowWaves(float2 p, float waveLength, float footprint, out float lost)
{
    float2 slope = 0;
    lost = 0;
    [unroll] for (uint k = 0; k < WATER_FLOW_WAVES; ++k)
    {
        const float lambda = waveLength / (1.0 + 0.4 * k);
        const float angle = 0.7 + 2.39996323 * k;
        const float2 direction = float2(cos(angle), sin(angle));
        const float keep = saturate(0.5 * lambda / max(footprint, 1e-6) - 1.0);
        // (six waves of equal slope amplitude a: the slope's variance is 6 a^2 / 2 - a = 1 / sqrt 3 for unit rms)
        slope += direction * (keep * 0.57735027 * cos(6.2831853 / lambda * dot(direction, p) + 1.7 * k));
        lost += (1.0 - keep * keep) * (1.0 / WATER_FLOW_WAVES);
    }
    return slope;
}

// The surface's unit normal with the flow's waves and the drain's funnel on it, for a sample at P (world) whose normal
// is n (out of the water, n.y > 0); footprint: the pixel's on the surface (m). variance: the slope variance the
// footprint removed.
float3 waterFlowNormal(WaterFlow f, float3 P, float3 n, float footprint, out float variance)
{
    const float2 d = P.xz - f.centre;
    const float2 l = float2(d.x * f.cosYaw - d.y * f.sinYaw, d.x * f.sinYaw + d.y * f.cosYaw);
    const float2 v = waterFlowVelocity(f, l);
    const float amount = f.waveSlope * saturate(length(v) / 0.2);
    float2 slope = 0;  // d eta / d local x, d eta / d local z
    variance = 0;
    if (amount > 0)
    {
        const float2 phase = frac(float2(f.cycle, f.cycle + 0.5));
        const float2 weight = 1.0 - abs(2.0 * phase - 1.0);
        float lost0, lost1;
        const float2 s0 = waterFlowWaves(l - v * ((phase.x - 0.5) * f.period), f.waveLength, footprint, lost0);
        const float2 s1 = waterFlowWaves(l - v * ((phase.y - 0.5) * f.period) + f.waveLength * float2(3.7, 6.1), f.waveLength, footprint, lost1);
        const float norm = 1.0 / max(dot(weight, weight), 1e-6);  // 1 / the weights' sum of squares
        slope = amount * sqrt(norm) * (weight.x * s0 + weight.y * s1);
        variance = amount * amount * norm * (weight.x * weight.x * lost0 + weight.y * weight.y * lost1);
    }
    if (f.circulation != 0)
    {
        const float2 away = l - f.drain;
        const float r = max(length(away), 0.03);
        const float rise = min(f.circulation * f.circulation / (387.2833 * r * r * r), 1.0);  // Gamma^2 / (4 pi^2 g r^3)
        slope += rise * away / max(length(away), 1e-6);
    }
    // local slopes to the world's, on the surface's own
    const float2 world = float2(slope.x * f.cosYaw + slope.y * f.sinYaw, slope.y * f.cosYaw - slope.x * f.sinYaw);
    const float2 own = -n.xz / n.y;
    const float2 total = own + world;
    return normalize(float3(-total.x, 1.0, -total.y));
}
#endif
