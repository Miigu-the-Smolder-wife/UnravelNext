#pragma once
// Volumetric clouds, GPU resources shared by the frame path and the tests (B5; CloudCommon.hlsli, CloudModel.h). Owner: S.
#include "CloudModel.h"
#include "unx/render/Device.h"

namespace unx::render::clouds
{
// The three noise textures, uploaded once (shape Texture3D RG8 128^3 and detail Texture3D RG8 32^3 with every mip: the
// noise, and the deviation of the level-0 texels under a mip's texel x 2 - CloudCommon.hlsli cloudDensityFiltered;
// weather Texture2D RG8 512^2; cirrus Texture2D R8 512^2 with every mip - the sheet is seen at grazing angles) with their
// SRVs.
struct CloudTextures
{
    ComPtr<ID3D12Resource> shape, detail, weather, cirrus;
    uint32_t shapeSrv = 0, detailSrv = 0, weatherSrv = 0, cirrusSrv = 0;
};
CloudTextures uploadTextures(Device& device, const CloudNoise& noise);
void releaseTextures(Device& device, CloudTextures& t);

// The cloud record (CloudCommon.hlsli layout, 240 B).
struct CloudRecord
{
    float base, top, coverage, sigmaMax;
    float albedo, detailStrength, g0, g1;
    float lobeBlend, invShape, invDetail, invWeather;
    float shapeOffset[3], bottomRadius;
    float detailOffset[3];
    uint32_t skySrv;  // the sky dome (the layer seen from the camera in every direction; R's escaping rays)
    float weatherOffset[2];
    uint32_t layerSrv, distanceSrv;  // the frame's cloud layer textures (readers; CloudSystem.cpp)
    float origin[3], pad2;
    uint32_t shape, detail, weather, shadow;
    float sunDir[3], shadowHalfExtent;
    float shadowCentre[3], shadowTexels;
    float sunIlluminance[3], skyRadianceTest;  // skyRadianceTest: CloudMarch mode 4 only (tests; the frame takes the sky from the air)
    float cirrusAltitude, cirrusOpticalDepth, cirrusCoverage, invCirrus;  // the cirrus sheet (coverage 0: none)
    float cirrusOffset[2];
    uint32_t cirrus;       // its map's SRV
    float powder;          // atmosphere.clouds.powder (the frame; 0: the fitted octaves alone)
    float flashPosition[3], flashRadius;   // a lightning flash (FrameContext::lightning; renderer space, m)
    float flashIntensity[3], pad3;         // cd x colour (0: none)
};
static_assert(sizeof(CloudRecord) == 240);
// shadow: the deep opacity map's SRV (filled per frame); sunDir: unit, toward the sun; shadow area: centre (renderer
// space), half extent (m), texels per side.
CloudRecord makeRecord(const CloudLayer& layer, const CloudOffsets& offsets, const CloudTextures& textures, double bottomRadius, const float sunDir[3],
                       const float sunIlluminance[3], const float shadowCentre[3], float shadowHalfExtent, uint32_t shadowTexels);
} // namespace unx::render::clouds
