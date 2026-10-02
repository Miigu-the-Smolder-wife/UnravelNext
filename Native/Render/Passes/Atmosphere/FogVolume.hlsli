// The height fog's own volume (atmosphere.fog; the reference's volumetric fog in front of its exponential height fog:
// ue6-main VolumetricFog.usf, HeightFogCommon.ush for the structure and the default numbers; the code is ours).
// The first fog was a medium of the air volume (Fog.hlsli fogMedium in FroxelIntegrate.hlsl): that grid is 24 px tiles and
// 64 slices out to 65 km - 22 slices in the first 30 m - and it showed as bands on a ceiling full of lights, and the
// air's sky term carried the fog over the whole sky (the batch of 2026-10-02). This volume is the near field alone:
//   grid      cells of cellPx pixels, z slices out to farM: slice(d) = log2(d k + 1) b with k = 32 / farM (the reference's
//             depth distribution scale 32): 9 cm slices at the camera, 3 m at 80 m with 96 slices;
//   scatter   FogScatter.hlsl, per cell: extinction (1/m) and the light scattered toward the camera per metre (nits/m) at
//             a point jittered inside the cell each frame - the sun through the fog's phase function outside the
//             casters' shadow (the cell's segment of its centre ray on the shadow pages VsmMarkFog.hlsl asked for), the
//             local lights (the air grid's sampled fluence and moment, read between its froxels), the indirect light
//             (the previous frame's Lumen translucency volume) - blended with the cell's history (0.9);
//   integrate FogIntegrate.hlsl, per column front to back: in-scattered radiance and transmittance at each slice's far
//             face; then zFar more slices from farM to farEndM (equal steps of log depth): the exponential height fog in
//             closed form (Fog.hlsli fogOpticalDepth) scattering the column's far source - the sun without casters (the
//             shadow pages are not asked for out there) and the indirect light at farM;
//   read      fogAt: one fetch of the integrated volume between its cells. The main view's air lookups (Atmosphere.hlsli
//             atmosphereAerial / atmosphereAirView) add it, so every layer that takes the air - opaque pixels, the
//             coverage layer, glass, water, particles - takes the fog at its own depth, and what removes the air from a
//             colour (the screen traces' scene colour) removes the fog with it. The parameters reach the kernels through
//             the frame constants (g_fog: a record's SRV + 1). Sky pixels take sky_amount of it (ShadeSky.hlsl; 1: the
//             sky through the fog as every pixel - the reference's fog pass covers the sky too).
// Units: world metres; radiance in nits (not exposed: the history outlives exposure changes).
#ifndef UNX_FOG_VOLUME_HLSLI
#define UNX_FOG_VOLUME_HLSLI
#include "Passes/Atmosphere/Fog.hlsli"

struct FogGrid
{
    uint x, y, z, cellPx;
    uint zFar;        // slices of the integrated volume past farM (the closed-form fog out to farEndM)
    float farM, k, b;
    float farEndM;
};
// a = { x | y << 16, z | cellPx << 16 | zFar << 24, asuint(farM), asuint(k) }, bBits = asuint(b), farEndBits = asuint(farEndM)
FogGrid fogGrid(uint4 a, uint bBits, uint farEndBits = 0)
{
    FogGrid g;
    g.x = a.x & 0xFFFFu;
    g.y = a.x >> 16;
    g.z = a.y & 0xFFFFu;
    g.cellPx = (a.y >> 16) & 0xFFu;
    g.zFar = a.y >> 24;
    g.farM = asfloat(a.z);
    g.k = asfloat(a.w);
    g.b = asfloat(bBits);
    g.farEndM = asfloat(farEndBits);
    return g;
}
// The far slices' boundaries (view depth): slice i of zFar spans fogFarDepth(i) .. fogFarDepth(i + 1).
float fogFarDepth(FogGrid g, float i) { return g.farM * exp2(i / float(g.zFar) * log2(g.farEndM / g.farM)); }
// Continuous slice coordinate of a view depth (0 at the camera, g.z at farM) and its inverse.
float fogSliceOfDepth(FogGrid g, float depth) { return log2(max(depth, 0.0) * g.k + 1.0) * g.b; }
float fogDepthOfSlice(FogGrid g, float slice) { return (exp2(slice / g.b) - 1.0) / g.k; }
// The depth pyramid's level whose texels are the cells' pixels (level 0 is half resolution).
uint fogHizMip(FogGrid g) { return (uint)max(firstbithigh(g.cellPx), 1) - 1u; }
// A cell's lateral width at a view depth (m).
float fogCellWidth(FogGrid g, float depth) { return g.cellPx * 2.0 * depth * g_tanHalfFovY / g_viewHeight; }

// A cell's segment of its centre ray (view depths za < zb) kept in front of a surface at 'limit' on that ray: what
// would lie behind moves the whole segment toward the camera (FogScatter.hlsl walks it, VsmMarkFog.hlsl asks for its
// pages: both through this).
void fogSegment(inout float za, inout float zb, float limit)
{
    const float behind = max(zb - limit, 0.0);
    zb -= behind;
    za = max(za - behind, 0.0);
}

// The fog's extinction (1/m) at a height (the medium's density under its height held to 64 x, as fogOpticalDepth).
float fogExtinctionAt(FogMedium f, float y)
{
    return f.density * min(exp2(-f.falloff * (y - f.height)), 64.0);
}

// The density's variation about its mean (atmosphere.fog.noise_amount): value noise of two octaves on a lattice that
// repeats every 256 points (zero mean, in [-1, 1]), twice as fine in height as across (fog lies in sheets).
// lattice: the position's lattice coordinates (FroxelSystem.cpp recordFogVolume: position x (1, 2, 1) / scale + the
// frame's offset - the world's origin and the wind's drift).
float fogLatticeValue(int3 c)
{
    uint h = (uint(c.x) & 255u) | (uint(c.y) & 255u) << 8 | (uint(c.z) & 255u) << 16;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return float(h & 0xFFFFu) * (2.0 / 65535.0) - 1.0;
}
float fogValueNoise(float3 x)
{
    const float3 base = floor(x);
    const float3 f = x - base;
    const float3 u = f * f * (3.0 - 2.0 * f);
    const int3 c = int3(base);
    const float x00 = lerp(fogLatticeValue(c), fogLatticeValue(c + int3(1, 0, 0)), u.x);
    const float x10 = lerp(fogLatticeValue(c + int3(0, 1, 0)), fogLatticeValue(c + int3(1, 1, 0)), u.x);
    const float x01 = lerp(fogLatticeValue(c + int3(0, 0, 1)), fogLatticeValue(c + int3(1, 0, 1)), u.x);
    const float x11 = lerp(fogLatticeValue(c + int3(0, 1, 1)), fogLatticeValue(c + int3(1, 1, 1)), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}
// The factor on the mean density at a lattice coordinate: 1 + amount x 2 x noise, not below 0 (amount <= 0.5 never
// reaches 0: the mean stays the closed form's).
float fogDensityScale(float3 lattice, float amount)
{
    if (!(amount > 0)) return 1.0;
    const float n = (fogValueNoise(lattice) + 0.5 * fogValueNoise(lattice * 2.0 + float3(37.0, 17.0, 59.0))) * (1.0 / 1.5);
    return max(1.0 + amount * 2.0 * n, 0.0);
}

// The view's fog record (FroxelSystem.cpp FogParamsGpu; 80 B). The first 32 bytes are all a reader needs.
struct FogParams
{
    uint slices;              // z | cell px << 16 | zFar << 24 (0: the volume is not there this frame)
    uint volumeSrv;           // the integrated volume (Texture3D, z + zFar slices)
    float k, b;               // slice(depth) = log2(depth k + 1) b up to farM
    float farM, farScale;     // past farM: z + log2(depth / farM) farScale (farScale = zFar / log2(far end / farM))
    float2 uvScale;           // view size / (grid x cell px)
    float skyAmount;
    float density, falloff, height, g;
    float3 albedo;
    float start;
    uint grid;                // x | y << 16 (15 bits each) | atmosphere.fog.on_rays << 31
    float farEndM;
    uint pad;
};
// false: the view has no fog (g_fog 0), or its volume is not there this frame.
bool fogLoad(out FogParams p)
{
    p = (FogParams)0;
    if (g_fog == 0) return false;
    ByteAddressBuffer record = ResourceDescriptorHeap[g_fog - 1];
    p = record.Load<FogParams>(0);
    return p.slices != 0;
}

// The fog between the camera and a view depth: rgb = in-scattered radiance (nits), a = transmittance.
// integrated: FogIntegrate.hlsl's volume (a texel = the integral to its slice's far face; z + zFar slices). uv: the
// pixel's place in the view, [0, 1]^2.
float4 fogVolumeAt(Texture3D<float4> integrated, FogGrid g, float2 uv, float depth)
{
    float c = fogSliceOfDepth(g, min(depth, g.farM));
    if (depth > g.farM && g.zFar != 0) c = float(g.z) + log2(min(depth, g.farEndM) / g.farM) * (float(g.zFar) / log2(g.farEndM / g.farM));
    const float2 scale = float2(g_viewWidth, g_viewHeight) / float2(g.x * g.cellPx, g.y * g.cellPx);
    float4 v = integrated.SampleLevel(g_linearClamp, float3(uv * scale, (max(c, 1.0) - 0.5) / float(g.z + g.zFar)), 0);
    if (c < 1.0) v = float4(v.rgb * c, lerp(1.0, v.a, c));  // (inside the first slice: from nothing at the camera)
    return v;
}

// The fog between the camera and a point of the main view (uv in [0, 1]^2, view depth in m; the sky: any depth past
// the far slices' end): rgb = in-scattered radiance (nits, not exposed), a = transmittance. No fog: (0, 0, 0, 1).
// (fogVolumeAt from the record's first 32 bytes: the air lookups carry this into kernels near the size limit.)
float4 fogAt(float2 uv, float depth)
{
    if (g_fog == 0) return float4(0, 0, 0, 1);
    ByteAddressBuffer record = ResourceDescriptorHeap[g_fog - 1];
    const uint4 a = record.Load4(0);
    if (a.x == 0) return float4(0, 0, 0, 1);
    const float4 f = asfloat(record.Load4(16));
    const float z = float(a.x & 0xFFFFu);
    const float c = depth <= f.x ? log2(depth * asfloat(a.z) + 1.0) * asfloat(a.w) : z + log2(depth / f.x) * f.y;
    Texture3D<float4> integrated = ResourceDescriptorHeap[a.y];
    float4 v = integrated.SampleLevel(g_linearClamp, float3(uv * f.zw, (max(c, 1.0) - 0.5) / (z + float(a.x >> 24))), 0);  // (past the last slice: clamped)
    if (c < 1.0) v = float4(v.rgb * c, lerp(1.0, v.a, c));
    return v;
}
// The fog over a ray that leaves a surface the view sees (reflection rays; radiance in nits): the transmittance of the
// height fog's closed form along the ray (the mean medium: no variation, no start distance), and as the fog's light the
// mean source of the view's own path to that surface - the volume's in-scattering there over its opacity, so what the
// cells around the surface hold (the sun outside the casters' shadow, the local lights, the indirect light): a room's
// fog stays the room's. Where the view's path is too thin to tell its source (opacity under 0.4 %) the ray is left as it
// is. uv, depth: the surface's place in the main view; t: the ray's length (a miss: any length past the fog).
float3 fogOverRay(float2 uv, float depth, float3 origin, float3 dir, float t, float3 radiance)
{
    FogParams p;
    if (!fogLoad(p) || (p.grid >> 31) == 0) return radiance;  // (bit 31 of the grid word: atmosphere.fog.on_rays)
    FogMedium m;
    m.on = true;
    m.density = p.density;
    m.falloff = p.falloff;
    m.height = p.height;
    m.g = p.g;
    m.albedo = p.albedo;
    m.start = 0;
    const float T = exp(-fogOpticalDepth(m, origin, dir, 0.0, t));
    if (T > 0.999) return radiance;
    const float4 v = fogAt(uv, depth);
    const float opacity = 1.0 - v.a;
    if (opacity < 4e-3) return radiance;
    return radiance * T + v.rgb * ((1.0 - T) / opacity);
}

// The air's in-scattering and transmittance with the fog in front of and inside it (the fog is the nearer, denser
// medium: the air's light is taken through all of it).
void fogOverAir(float2 uv, float depth, inout float3 inscatter, inout float3 transmittance)
{
    if (g_fog == 0) return;
    const float4 fog = fogAt(uv, depth);
    inscatter = inscatter * fog.a + fog.rgb;
    transmittance *= fog.a;
}
#endif
