// Height fog as a medium of the air volume (atmosphere.fog; the reference's exponential height fog with volumetric fog:
// ue6-main HeightFogCommon.ush, VolumetricFog.usf for the structure - density falling by a power of two with height,
// albedo, extinction scale, a Henyey-Greenstein phase function; the code is ours).
// The air volume already integrates media per froxel slice (the particle media: optical depth and self-attenuated
// source, FroxelIntegrate.hlsl); the fog is one more such medium, present in every slice:
//   optical depth  the integral of the extinction along the slice's segment of the tile ray (closed form: the density is
//                  exponential in height);
//   source         what the fog scatters toward the camera, per unit of its own optical depth: albedo x (the sun through
//                  the fog's phase function, less the fraction of the segment in the casters' shadow - the same shadowed
//                  fraction the air's single scattering uses, so shafts of light come out of the sun's shadow map; the
//                  local lights from the froxels' sampled fluence and direction moment; the indirect light from the
//                  Lumen translucency volume of the previous frame).
// Unlike the atmosphere (atmosphere.aerial_start_m) the fog starts at the camera (or at start distance).
#ifndef UNX_FOG_HLSLI
#define UNX_FOG_HLSLI

struct FogMedium
{
    bool on;
    float density;   // extinction (1/m) at 'height'
    float falloff;   // the density halves every 1 / falloff metres of height
    float height;    // m (scene y)
    float g;         // Henyey-Greenstein asymmetry
    float3 albedo;   // scattering / extinction
    float start;     // m from the camera along the ray: no fog before
    // A second layer of the same medium (the reference's SecondFogData: its own density, falloff and height; the albedo
    // and the phase function are the medium's) - a low ground fog under a thin haze, the rain's veil. density2 0: none.
    float density2, falloff2, height2;
};
// a = { bit 0: on (bit 1: FROXEL_CLIP_AT_SURFACE, FroxelSlice.hlsli), ... }, b = { density, falloff, height, g } (floats),
// c = { albedo r, g, b, start distance } (floats), d = { the second layer's density, falloff, height, 0 } (floats)
FogMedium fogMedium(uint4 a, uint4 b, uint4 c, uint4 d = uint4(0, 0, 0, 0))
{
    FogMedium f;
    f.on = (a.x & 1u) != 0;
    f.density = asfloat(b.x);
    f.falloff = asfloat(b.y);
    f.height = asfloat(b.z);
    f.g = asfloat(b.w);
    f.albedo = asfloat(c.xyz);
    f.start = asfloat(c.w);
    f.density2 = asfloat(d.x);
    f.falloff2 = asfloat(d.y);
    f.height2 = asfloat(d.z);
    return f;
}

// The fog's optical depth along origin + dir t, t in [t0, t1] (dir unit): the integral of the fog's extinction
// (FogVolume.hlsli fogExtinctionAt: the density under the fog's height is held to 64 x the density at it), itself held
// to 64 (nothing is seen through either).
// One layer's (density at 'height', halving every 1 / falloff metres): not held to 64.
float fogLayerDepth(float density, float falloff, float height, float3 origin, float3 dir, float t0, float t1)
{
    if (!(density > 0)) return 0;
    // the extinction along the segment is density x min(e^x, 64), x = -k x (the height above the fog's) linear in t
    const float k = falloff * 0.69314718;  // 2^(-falloff h) = e^(-k h)
    const float xa = -k * (origin.y + dir.y * t0 - height), xb = -k * (origin.y + dir.y * t1 - height);
    const float lo = min(xa, xb), hi = max(xa, xb), held = 4.1588831;  // ln 64: from there down the density is held
    // mean of min(e^x, 64) over [lo, hi]: the stretch [l, u] above the hold integrates to e^u (1 - e^-(u - l)) (in the
    // form that stays exact where the height barely changes), the stretch at the hold to 64 x its length
    float mean = min(exp(lo), 64.0);
    if (hi - lo > 1e-6)
    {
        const float l = min(lo, held), u = min(hi, held), d = u - l;
        const float above = exp(u) * (d > 1e-2 ? 1.0 - exp(-d) : d * (1.0 - d * (0.5 - d * (1.0 / 6.0))));
        mean = (above + 64.0 * (max(hi, held) - max(lo, held))) / (hi - lo);
    }
    return density * mean * (t1 - t0);
}
float fogOpticalDepth(FogMedium f, float3 origin, float3 dir, float t0, float t1)
{
    t0 = max(t0, f.start);
    if (!f.on || !(t1 > t0)) return 0;
    float tau = fogLayerDepth(f.density, f.falloff, f.height, origin, dir, t0, t1);
    if (f.density2 > 0) tau += fogLayerDepth(f.density2, f.falloff2, f.height2, origin, dir, t0, t1);
    return min(tau, 64.0);
}

// atmosphere.fog.sun_through_fog: the share of the sun's light that reaches p as direct light through the height fog -
// exp(-tau (1 - g)), tau the fog's optical depth from p toward the sun (the closed form: the mean medium from p on) and
// g its phase function's asymmetry: light scattered forward stays in the beam (the similarity relation's reduced
// extinction). sun: unit, toward the sun.
float fogSunThrough(FogMedium f, float3 p, float3 sun)
{
    FogMedium m = f;
    m.on = true;
    m.start = 0;
    return exp(-fogOpticalDepth(m, p, sun, 0.0, 1.0e6) * (1.0 - f.g));
}
#endif
