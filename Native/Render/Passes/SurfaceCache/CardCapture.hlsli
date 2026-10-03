// The mesh card capture (surface_cache.mesh_cards; SurfaceCacheCards.cpp): a card page's orthographic view of its own
// instance, drawn from the mesh's source triangles into the frame's capture atlas - what Unreal's Lumen card capture
// draws (LumenSceneCardCapture.cpp: the primitive's mesh, orthographic along the card's direction, into albedo, normal,
// emissive and depth), in this renderer's records. One DispatchMesh per (page, submesh): the viewport is the page's
// rectangle in the capture atlas, depth 0 = the card's front (the side it is seen from), 1 = its back; the rasteriser's
// depth clip removes what lies outside the card's depth range and LESS keeps the surface nearest the front.
// Root constants of both stages:
//   P[0] = { scene instance, the submesh's first index (mesh-relative), the submesh's triangles, material }
//   P[1] = { asuint(card origin xyz: mesh card space - the mesh's axes, metres), asuint(the instance's uniform scale) }
//   P[2] = { asuint(card half sizes xyz along the card's axes), direction | flags << 8 (bit 0: two-sided material) }
//   P[3] = asuint(the page's rectangle in the card's uv: min xy, max zw)
//   P[4] = { M's texture table SRV (UNX_NONE: material constants and the published textures only), 0, 0, 0 }
#ifndef UNX_CARD_CAPTURE_HLSLI
#define UNX_CARD_CAPTURE_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"

struct CardVertex
{
    float4 position : SV_Position;
    float3 normal : NORMAL;     // card space: x, y along the card's face, z toward the side the card is seen from
    float4 tangent : TANGENT;   // card space; w = the bitangent's sign in card space (the mesh's sign x the basis' handedness)
    float2 uv : TEXCOORD0;
    float3 scaled : SCALEDPOS;  // the mesh-space point x the instance's scale (metres): triplanar materials
    float2 uv1 : TEXCOORD1;     // the mesh's second uv set (its first for a mesh without one) and its vertex colour
    float4 color : COLOR0;      // (white without): the material inputs' streams (Scene.hlsli GpuMaterialInputs)
};

struct CardPrimitive
{
    bool cull : SV_CullPrimitive;
};

// The card's axes in mesh space (scene::meshCardAxes): directions -X, +X, -Y, +Y, -Z, +Z.
void ccAxes(uint direction, out float3 x, out float3 y, out float3 z)
{
    const uint axis = direction >> 1;
    const float s = (direction & 1u) != 0 ? 1.0 : -1.0;
    x = axis == 0 ? float3(0, 1, 0) : float3(1, 0, 0);
    y = axis == 2 ? float3(0, 1, 0) : float3(0, 0, 1);
    z = axis == 0 ? float3(s, 0, 0) : (axis == 1 ? float3(0, s, 0) : float3(0, 0, s));
}

#endif
