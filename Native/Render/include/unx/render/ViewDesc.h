#pragma once
// A camera view (INTERFACES_KO.md 5.1). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/core/Math.h"
#include "unx/render/GraphTypes.h"
#include "unx/render/ViewKind.h"

namespace unx::scene
{
struct Camera;
}

namespace unx::render
{
// A camera view. The main view comes from the scene camera; R builds planar-reflection views (5.4).
struct ViewDesc
{
    gpu::ViewKind kind = gpu::ViewKind::Main;
    uint32_t width = 0, height = 0;     // render target size of this view
    float4x4 view, proj, viewProj, prevViewProj, invViewProj;
    float3 position{};
    float nearPlane = 0.05f;
    float verticalFov = 1.0471976f;
    float4 clipPlane{};                 // world plane (xyz normal, w offset); keep dot(n,p) + w >= 0; zero = none.
                                        // V honours it with SV_ClipDistance0 and in cluster culling.
    bool mirrored = false;              // reflection views: front faces wind clockwise (V swaps cull mode)
    float ev100 = 14.0f;
    // Planar reflection views (v1.22, R request; v1.28 apron, M request): which pixels are drawn. R8_UINT, width x
    // height: 1 = mirror pixel (R reads it), 2 = apron (the 3 x 3 neighbourhood of mirror pixels: drawn and shaded so
    // edge detection and composite at the mirror's border see the real reflected surfaces; R does not read it), 0 =
    // skipped; invalid = every pixel. V, M and S draw every nonzero pixel: V culls clusters over tiles without drawn
    // pixels and fills the others' depth with the nearest value before the raster (they stay VIS_NONE).
    TextureRef planarMask;              // [R]
    TextureRef planarTileMask;          // optional R8_UINT ceil(W/8) x ceil(H/8), nonzero = the tile has drawn [R]
                                        // pixels of the dilated mask (M and S tile classification in one load)

    // Main view from a scene camera (reversed-Z infinite projection, Math.h).
    static ViewDesc fromCamera(const scene::Camera& camera, uint32_t width, uint32_t height, const float4x4& prevViewProj);
    // Mirror of 'mainView' across 'plane' (world), cropped to pixel rectangle 'region' of the main view (the planar
    // reflector's screen bounds, A_r): off-centre projection, oblique clip plane, same exposure. Owner of use: R.
    static ViewDesc planarReflection(const ViewDesc& mainView, float4 plane, uint32_t regionX, uint32_t regionY, uint32_t regionWidth, uint32_t regionHeight);
};
} // namespace unx::render
