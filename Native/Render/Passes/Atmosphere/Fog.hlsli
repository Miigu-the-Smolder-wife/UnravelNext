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
// Unlike the atmosphere (AIR_VIEW_START_M) the fog starts at the camera (or at start distance).
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
};
// a = { bit 0: on (bit 1: FROXEL_CLIP_AT_SURFACE, FroxelSlice.hlsli), ... }, b = { density, falloff, height, g } (floats),
// c = { albedo r, g, b, start distance } (floats)
FogMedium fogMedium(uint4 a, uint4 b, uint4 c)
{
    FogMedium f;
    f.on = (a.x & 1u) != 0;
    f.density = asfloat(b.x);
    f.falloff = asfloat(b.y);
    f.height = asfloat(b.z);
    f.g = asfloat(b.w);
    f.albedo = asfloat(c.xyz);
    f.start = asfloat(c.w);
    return f;
}

// The fog's optical depth along origin + dir t, t in [t0, t1] (dir unit). The density under the fog's height is held to
// 64 x the density at it, and a slice's optical depth to 64 (nothing is seen through either).
float fogOpticalDepth(FogMedium f, float3 origin, float3 dir, float t0, float t1)
{
    t0 = max(t0, f.start);
    if (!f.on || !(t1 > t0) || !(f.density > 0)) return 0;
    const float k = f.falloff * 0.69314718;  // 2^(-falloff h) = e^(-k h)
    const float h0 = origin.y + dir.y * t0 - f.height, h1 = origin.y + dir.y * t1 - f.height;
    const float x0 = -k * h0, x1 = -k * h1;
    if (min(x0, x1) >= 60.0) return min(f.density * 64.0 * (t1 - t0), 64.0);  // (far under the fog's height: the held density)
    const float e0 = exp(clamp(x0, -80.0, 60.0)), e1 = exp(clamp(x1, -80.0, 60.0));
    const float a = k * (h1 - h0);
    // mean of e^(-k h) over the segment: (e0 - e1) / a, or the ends' mean where the height barely changes
    const float mean = abs(a) > 1e-3 ? (e0 - e1) / a : 0.5 * (e0 + e1);
    return min(f.density * clamp(mean, 0.0, 64.0) * (t1 - t0), 64.0);
}
#endif
