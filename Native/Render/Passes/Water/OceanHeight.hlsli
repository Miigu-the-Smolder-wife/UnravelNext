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

#define OH_N 512u
#define OH_CELLS 511u
#define OH_MIPS 9u
#define OH_ITERATIONS 2  // Newton steps (from the scatter mesh's x0)
#define OH_RESIDUAL 0.02  // of s_l

float ohS0() { return asfloat(P[1].z); }
float ohLevelSpacing(uint level) { return ohS0() * float(1u << level); }
int2 ohOrigin(uint level, float2 camera) { return int2(floor(camera / ohLevelSpacing(level))) - int2(256, 256); }
float ohCascadeLength(uint c) { return asfloat(c == 0 ? P[2].x : (c == 1 ? P[2].y : P[2].z)); }

// Displacement (Dx, h, Dz) of the ocean at rest position x0, low-passed to spacing s (each cascade's mip whose texel is
// s, trilinear). Texel (x, z) of mip 0 is the rest position (x, z) L / 512 (Ocean.h), so uv = x0 / L + 0.5 / 512 on
// every mip.
float3 ohDisplacement(float2 x0, float s)
{
    Texture2DArray<float4> field = ResourceDescriptorHeap[P[0].x];
    float3 d = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = ohCascadeLength(c);
        const float mip = clamp(log2(s * 512.0 / L), 0.0, 9.0);
        d += field.SampleLevel(g_linearWrap, float3(x0 / L + 0.5 / 512.0, c), mip).xyz;
    }
    return d;
}
// The same low-passed field with its Jacobian: (Dx, h, Dz) and (dDx/dx, dDx/dz, dDz/dz) (dDz/dx = dDx/dz: the horizontal
// displacement is a gradient field).
void ohDisplacementJacobian(float2 x0, float s, out float3 d, out float3 j)
{
    Texture2DArray<float4> field = ResourceDescriptorHeap[P[0].x];
    Texture2DArray<float4> slopes = ResourceDescriptorHeap[P[3].w];
    d = 0;
    j = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = ohCascadeLength(c);
        const float mip = clamp(log2(s * 512.0 / L), 0.0, 9.0);
        const float3 uv = float3(x0 / L + 0.5 / 512.0, c);
        const float4 a = field.SampleLevel(g_linearWrap, uv, mip), b = slopes.SampleLevel(g_linearWrap, uv, mip);
        d += a.xyz;
        j += float3(b.z, a.w, b.w);
    }
}
#endif
