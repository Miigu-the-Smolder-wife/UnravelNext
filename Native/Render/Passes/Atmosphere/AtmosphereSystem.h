#pragma once
// S track, atmosphere (ARCHITECTURE 2.3): persistent LUTs (transmittance, multiple scattering, sky view, aerial
// perspective) in the renderer's track state (key "s.atmosphere"), rebuilt only when their inputs change, imported into every frame's graph and
// published in FrameResources. Internal to Passes/Atmosphere; the public face is Atmosphere.hlsli.
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::atmosphere
{
// Mirror of AtmosphereParams (AtmosphereCommon.hlsli), 144 B.
struct AtmosphereParams
{
    float bottomRadius, topRadius, rayleighScaleHeight, mieScaleHeight;
    float3 rayleighScattering;
    float mieG;
    float3 mieScattering;
    float ozoneCenter;
    float3 mieAbsorption;
    float ozoneWidth;
    float3 ozoneAbsorption;
    float froxelFarM;
    float3 groundAlbedo;
    uint32_t froxelSlices;
    uint32_t transmittanceSize[2];
    uint32_t multiScatterSize[2];
    uint32_t skyViewSize[2];
    uint32_t transmittanceSteps, multiScatterDirections;
    uint32_t multiScatterSteps, skySegments, froxelTilePx;
    float froxelNearM;
};
static_assert(sizeof(AtmosphereParams) == 144);

// Model parameters from the scene, LUT sizes and quadrature counts from Config/quality/atmosphere.toml; the froxel grid
// of the air volume (built by S's froxels(), FroxelSystem) so its lookups need no other input.
AtmosphereParams makeParams(const scene::Atmosphere& atmosphere, const QualityConfig& quality);

struct AtmosphereStats
{
    uint64_t lutBuilds = 0;       // transmittance + multiple scattering
    uint64_t skyViewBuilds = 0;
};

// Declares this frame's atmosphere passes and fills fc.resources.{transmittanceLut, multiScatterLut, skyViewLut}.
// aerialPerspective is the air volume that froxels() builds on the froxel grid (it needs the VSM).
void record(FramePassContext& fc);
const AtmosphereStats& stats(TrackState& state);
} // namespace unx::render::atmosphere
