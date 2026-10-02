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
TextureRef postBloomTail(FramePassContext& fc, TextureRef hdr, uint32_t levels, const PostLocalExposure& localExposure = {});
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
