#pragma once
// Volumetric clouds, GPU resources shared by the frame path and the tests (B5; CloudCommon.hlsli, CloudModel.h). Owner: S.
#include "CloudModel.h"
#include "unx/render/Device.h"

namespace unx::render::clouds
{
// The three noise textures, uploaded once (shape Texture3D R8 128^3, detail Texture3D R8 32^3, weather Texture2D RG8
// 512^2) with their SRVs.
struct CloudTextures
{
    ComPtr<ID3D12Resource> shape, detail, weather;
    uint32_t shapeSrv = 0, detailSrv = 0, weatherSrv = 0;
};
CloudTextures uploadTextures(Device& device, const CloudNoise& noise);
void releaseTextures(Device& device, CloudTextures& t);

// The cloud record (CloudCommon.hlsli layout, 176 B).
struct CloudRecord
{
    float base, top, coverage, sigmaMax;
    float albedo, detailStrength, g0, g1;
    float lobeBlend, invShape, invDetail, invWeather;
    float shapeOffset[3], bottomRadius;
    float detailOffset[3], pad0;
    float weatherOffset[2], pad1[2];
    float origin[3], pad2;
    uint32_t shape, detail, weather, shadow;
    float sunDir[3], shadowHalfExtent;
    float shadowCentre[3], shadowTexels;
    float sunIlluminance[3], pad3;
};
static_assert(sizeof(CloudRecord) == 176);
// shadow: the deep opacity map's SRV (filled per frame); sunDir: unit, toward the sun; shadow area: centre (renderer
// space), half extent (m), texels per side.
CloudRecord makeRecord(const CloudLayer& layer, const CloudOffsets& offsets, const CloudTextures& textures, double bottomRadius, const float sunDir[3],
                       const float sunIlluminance[3], const float shadowCentre[3], float shadowHalfExtent, uint32_t shadowTexels);
} // namespace unx::render::clouds
