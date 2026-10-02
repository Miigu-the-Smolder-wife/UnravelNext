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
//             face, and the column's source beyond the volume (the sun without casters, the indirect light at farM);
//   apply     FogApply.hlsl over the lit opaque image: the volume to the pixel's depth, past farM the closed form of the
//             exponential height fog (Fog.hlsli fogOpticalDepth) with the column's far source. Sky pixels take
//             sky_amount of it (0: none - the reference fogs rendered opaque pixels only when a sky atmosphere is there).
// Units: world metres; radiance in nits (not exposed: the history outlives exposure changes).
#ifndef UNX_FOG_VOLUME_HLSLI
#define UNX_FOG_VOLUME_HLSLI
#include "Passes/Atmosphere/Fog.hlsli"

struct FogGrid
{
    uint x, y, z, cellPx;
    float farM, k, b;
};
// a = { x | y << 16, z | cellPx << 16, asuint(farM), asuint(k) }, bBits = asuint(b)
FogGrid fogGrid(uint4 a, uint bBits)
{
    FogGrid g;
    g.x = a.x & 0xFFFFu;
    g.y = a.x >> 16;
    g.z = a.y & 0xFFFFu;
    g.cellPx = a.y >> 16;
    g.farM = asfloat(a.z);
    g.k = asfloat(a.w);
    g.b = asfloat(bBits);
    return g;
}
// Continuous slice coordinate of a view depth (0 at the camera, g.z at farM) and its inverse.
float fogSliceOfDepth(FogGrid g, float depth) { return log2(max(depth, 0.0) * g.k + 1.0) * g.b; }
float fogDepthOfSlice(FogGrid g, float slice) { return (exp2(slice / g.b) - 1.0) / g.k; }
// The depth pyramid's level whose texels are the cells' pixels (level 0 is half resolution).
uint fogHizMip(FogGrid g) { return (uint)max(firstbithigh(g.cellPx), 1) - 1u; }
// A cell's lateral width at a view depth (m).
float fogCellWidth(FogGrid g, float depth) { return g.cellPx * 2.0 * depth * g_tanHalfFovY / g_viewHeight; }

// The fog's extinction (1/m) at a height (the medium's density under its height held to 64 x, as fogOpticalDepth).
float fogExtinctionAt(FogMedium f, float y)
{
    return f.density * min(exp2(-f.falloff * (y - f.height)), 64.0);
}

// The fog between the camera and a view depth inside the volume: rgb = in-scattered radiance (nits), a = transmittance.
// integrated: FogIntegrate.hlsl's volume (a texel = the integral to its slice's far face). uv: the pixel's place in the
// view, [0, 1]^2.
float4 fogVolumeAt(Texture3D<float4> integrated, FogGrid g, float2 uv, float depth)
{
    const float c = fogSliceOfDepth(g, min(depth, g.farM));
    const float2 scale = float2(g_viewWidth, g_viewHeight) / float2(g.x * g.cellPx, g.y * g.cellPx);
    float4 v = integrated.SampleLevel(g_linearClamp, float3(uv * scale, (max(c, 1.0) - 0.5) / float(g.z)), 0);
    if (c < 1.0) v = float4(v.rgb * c, lerp(1.0, v.a, c));  // (inside the first slice: from nothing at the camera)
    return v;
}
#endif
