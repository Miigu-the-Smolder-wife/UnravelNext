#pragma once
// S track, atmosphere (ARCHITECTURE 2.3): persistent LUTs (transmittance, the multiple-scattering source table J_ms,
// sky view, aerial perspective) in the renderer's track state (key "s.atmosphere"), rebuilt only when their inputs change, imported into every frame's graph and
// published in FrameResources. Internal to Passes/Atmosphere; the public face is Atmosphere.hlsli.
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::atmosphere
{
// Mirror of AtmosphereParams (AtmosphereCommon.hlsli), 176 B.
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
    uint32_t transmittanceSize[2];  // LUT texels; the texture has 2 more rows (ground indirect irradiance, this record)
    uint32_t multiScatterOrders, multiScatterShOrder;
    uint32_t skyViewSize[2];
    uint32_t transmittanceSteps, multiScatterDirections;
    uint32_t multiScatterSteps, skySegments, froxelTilePx;
    float froxelNearM;
    uint32_t multiScatterSize[4];  // J_ms table (nu, mu_s, mu, r)
    uint32_t multiScatterShGrid[2];  // density projection grid: elevation nodes per half, azimuth nodes over [0, pi]
    uint32_t clouds[2];  // B5: [0] SRV + 1 of the cloud record (0 = no clouds), [1] 0 (CloudSystem.cpp)
};
static_assert(sizeof(AtmosphereParams) == 176);

// Model parameters from the scene, LUT sizes and quadrature counts from Config/quality/atmosphere.toml; the froxel grid
// of the air volume (built by S's froxels(), FroxelSystem) so its lookups need no other input.
AtmosphereParams makeParams(const scene::Atmosphere& atmosphere, const QualityConfig& quality);

struct AtmosphereStats
{
    uint64_t lutBuilds = 0;       // transmittance + multiple-scattering table (J_ms, MsBuild.hlsl)
    uint64_t skyViewBuilds = 0;
};

// Declares this frame's atmosphere passes and fills fc.resources.{transmittanceLut, multiScatterLut, skyViewLut}.
// aerialPerspective is the air volume that froxels() builds on the froxel grid (it needs the VSM).
void record(FramePassContext& fc);
const AtmosphereStats& stats(TrackState& state);
} // namespace unx::render::atmosphere
