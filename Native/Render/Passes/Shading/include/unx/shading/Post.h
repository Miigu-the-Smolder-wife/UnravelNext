#pragma once
// M's HDR post chain (Post.cpp; FEATURES_GAME 4 and 6, render A item A4).
#include "unx/render/Frame.h"

namespace unx::render::shading
{
// A post term is on for this view (the main view of a display frame with shading.post_* set).
bool postActive(FramePassContext& fc, const ViewResources& view);
// A10: the view has glass on V's translucent layer (the main view's translucentVis): M composites it over the shaded
// float image (TranslucentComposite.hlsl), so the view is shaded linear and encoded by the post chain.
bool translucentActive(FramePassContext& fc, const ViewResources& view);
// The exposed-linear RGBA16F target the view is shaded into when the chain is on.
TextureRef postTarget(FramePassContext& fc, const ViewResources& view);
// The lens PSF's tail of 'hdr' at half resolution (energy-normalised pyramid over 'levels' octaves; tests read it).
// localExposure: 12 words for PostDownsample's P[1..3] (LocalExposure.hlsli: the first level's source texels take
// their local exposure), filled at execution; none: the image as it is.
struct PostLocalExposure
{
    TextureRef grid, blurred;
    float uvScale[2] = { 1, 1 };
    float highlight = 1, shadow = 1, detail = 1, blend = 0.6f, logMiddleGrey = -2.4739312f;
    bool valid() const { return grid.valid(); }
};
// Image-based lens flares (shading.post_lens_flare*; PostFlare.hlsl): the reference's lens flare settings with their
// defaults - the eight flares' tints, whose alpha gives each flare's scale about the centre ((alpha - 0.5) x 7).
struct PostLensFlare
{
    bool on = false;
    float intensity = 1, tint[3] = { 1, 1, 1 };
    float bokehSize = 3;   // percent of the view's width
    float threshold = 8;   // r + g + b of the exposed scene colour from which a pixel flares
    float halo = 0;        // (ours: a ring of the mirrored image; 0 = none, as the reference)
    uint32_t blades = 0;   // the aperture's shape: 0 a disc, else a polygon
    float tints[8][4] = { { 1.0f, 0.8f, 0.4f, 0.6f },  { 1.0f, 1.0f, 0.6f, 0.53f }, { 0.8f, 0.8f, 1.0f, 0.46f }, { 0.5f, 1.0f, 0.4f, 0.39f },
                          { 0.5f, 0.8f, 1.0f, 0.31f }, { 0.9f, 1.0f, 0.8f, 0.27f }, { 1.0f, 0.8f, 0.4f, 0.22f }, { 0.9f, 0.7f, 0.7f, 0.15f } };
};
// flare, flareOut: with flares on and at least three levels, *flareOut is the flares' image at quarter resolution
// (RGBA16F, what the final pass adds).
TextureRef postBloomTail(FramePassContext& fc, TextureRef hdr, uint32_t levels, const PostLocalExposure& localExposure = {}, const PostLensFlare* flare = nullptr,
                         TextureRef* flareOut = nullptr);
// Bloom, vignetting, tone curve, grading, grain, dither and encoding from 'hdr' into view.color.
void postChain(FramePassContext& fc, const ViewResources& view, TextureRef hdr);
// v1.91 camera white balance (defect queue 6): the 3 x 3 matrix (row-major, linear Rec.709 -> linear Rec.709) that
// adapts a scene lit by the illuminant of correlated colour temperature 'kelvin' and tint 'duv' (CIE 1960 Duv) to the
// display's D65 by the Bradford transform (XYZ scaling in the Bradford cone space; CIE 1931 2 degree, daylight locus
// from 4000 K, Planckian locus below). False with the identity when kelvin is 0 or the white is D65 with no tint
// (within 1 K): the chain then skips the multiply, bit-identical to the output without white balance.
bool whiteBalanceMatrix(float kelvin, float duv, float m[9]);
// The chromaticity (CIE 1931 xy) of that white point (tests).
void whiteBalanceChromaticity(double kelvin, double duv, double& x, double& y);
} // namespace unx::render::shading
