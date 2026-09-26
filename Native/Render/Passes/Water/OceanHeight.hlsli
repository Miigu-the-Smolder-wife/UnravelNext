// Water surface as a world-space height clipmap (FEATURES_GAME 1.8): level l (0..L-1) holds 512^2 lattice points of
// spacing s_l = s_0 2^l around the camera; point (i, j) of level l is world xz = (origin_l + (i, j)) s_l with
// origin_l = floor(camera / s_l) - 256 (snapped: the lattice never swims). Each point stores
//   (H, x0 - w (2), flags): H the displaced surface's height at w (the upper envelope where the surface folds), x0 the
//   rest position whose displaced point is w (shading reads slopes, foam and ripples there).
// Between points the surface is the bilinear interpolation of H. Cell (i, j) (points i..i+1, j..j+1; cells 0..510) has
// max/min mips: mip 0 = the extremes of its 4 corners, mip m = extremes of its 4 children (their point ranges share
// edges, so the union is exact): a node's max bounds the bilinear surface over it (bilinear <= max corner).
// Root constants (build and mip kernels):
//   P[0] displacement SRV (Ocean texture array, all mips), height UAV (Texture2DArray RGBA32F, L slices), bounds UAV
//        (Texture2DArray RG32F max/min, L slices, 9 mips: this dispatch's mip), levels L
//   P[1] camera x, camera z, s_0 (m), water level (m)
//   P[2] cascade lengths (m) 0..2, fold search radius R (m: the largest horizontal displacement)
//   P[3] fold counter UAV (raw), bounds source UAV (mip kernels), mip index, slopes SRV (Ocean texture array, all mips)
#ifndef UNX_WATER_OCEAN_HEIGHT_HLSLI
#define UNX_WATER_OCEAN_HEIGHT_HLSLI
#include "Bindless.hlsli"
#include "OceanSample.hlsli"

#define OH_N 512u
#define OH_CELLS 511u
#define OH_MIPS 9u
#define OH_ITERATIONS 2  // Newton steps (from the scatter mesh's x0)
#define OH_RESIDUAL 0.02  // of s_l

float ohS0() { return asfloat(P[1].z); }
float ohLevelSpacing(uint level) { return ohS0() * float(1u << level); }
int2 ohOrigin(uint level, float2 camera) { return int2(floor(camera / ohLevelSpacing(level))) - int2(256, 256); }
float ohCascadeLength(uint c) { return asfloat(c == 0 ? P[2].x : (c == 1 ? P[2].y : P[2].z)); }

// Displacement (Dx, h, Dz) of the ocean at rest position x0 for spacing s: OceanSample.hlsli (C1 Hermite below a
// cascade's texel, trilinear above: the same surface as the view grid's).
float3 ohDisplacement(float2 x0, float s)
{
    return oceanSample(P[0].x, P[3].w, float3(ohCascadeLength(0), ohCascadeLength(1), ohCascadeLength(2)), x0, s).D;
}
// The same with its Jacobian: d = (Dx, h, Dz), j = (dDx/dx, dDx/dz, dDz/dz) (dDz/dx = dDx/dz).
void ohDisplacementJacobian(float2 x0, float s, out float3 d, out float3 j)
{
    const OceanPoint o = oceanSample(P[0].x, P[3].w, float3(ohCascadeLength(0), ohCascadeLength(1), ohCascadeLength(2)), x0, s);
    d = o.D;
    j = float3(o.dDdx.x, o.dDdz.x, o.dDdz.z);
}
#endif
