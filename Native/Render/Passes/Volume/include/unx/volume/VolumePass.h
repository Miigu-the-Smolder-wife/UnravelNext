#pragma once
// Particle media (smoke, fire volumes) and heat haze of a view (track E; VolumeCommon.hlsli): from the FX particle module's
// latest ticks, interpolated to the frame time like the sprites.
//   media: froxel-tile lists of the volume particles and volumeSlices (per slice optical depth and source) for S's froxel
//          integration (request 20260925_FX_particle_render_rules 3b);
//   haze:  1/4-resolution deflection field and front depth for M's displaced lookup (FEATURES_GAME 0.A-8).
#include "unx/fx/ParticleLayer.h"
#include "unx/fx/Particles.h"

#include <cstdint>
#include <wrl/client.h>

namespace unx::volume
{
struct VolumeFrame
{
    const render::ViewDesc* view = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    double camera[3] = {};        // renderer world (frame + FrameContext::worldOrigin)
    float streamAxes[3] = { 1, 1, 1 };  // FrameContext::streamAxes: stream (VFX World) -> renderer axis signs
    double time = 0;              // the particle stream's context time of this frame (s)
    fx::ParticleLighting lighting;  // media lighting (the lit sprites' inputs)
    render::BufferRef froxelLights;  // S's lists of this view (FroxelGrid header): required for media
    bool media = false, haze = false;
};

struct VolumeOutput
{
    bool valid = false;  // false: no particle tick yet
    render::TextureRef volumeSlices;                      // media (invalid without)
    render::TextureRef distortionOffset, distortionDepth;  // haze (invalid without)
    // the pass's own resources (tests read them)
    render::BufferRef constants, records, counters, mediaCounts, mediaStarts, mediaEntries, hazeCounts, hazeStarts, hazeEntries;
    uint32_t threads = 0, gridX = 0, gridY = 0, slices = 0, tilePx = 0, hazeWidth = 0, hazeHeight = 0;
    uint32_t mediaEntryCapacity = 0, hazeEntryCapacity = 0;
    float w = 0;
};

// Froxel grid dimensions of a view, S's formula (FroxelSystem.cpp froxelGridFor: quality atmosphere.froxels.*).
void froxelGridSize(const QualityConfig& quality, uint32_t width, uint32_t height, uint32_t& gridX, uint32_t& gridY, uint32_t& slices, uint32_t& tilePx);

class VolumePass
{
public:
    explicit VolumePass(render::Device& device);
    ~VolumePass();
    VolumePass(const VolumePass&) = delete;
    VolumePass& operator=(const VolumePass&) = delete;

    VolumeOutput record(fx::ParticleSystem& particles, render::RenderGraph& graph, render::ShaderLibrary& shaders, const QualityConfig& quality,
                        uint64_t importIndex, const VolumeFrame& frame);
    // Tests and tools: the same lists, slices and haze field from given records (VolumeRecord, 48 B each; kinds as in
    // VolumeCommon.hlsli) instead of the particle module's particles.
    VolumeOutput recordRecords(render::BufferRef records, uint32_t count, render::RenderGraph& graph, render::ShaderLibrary& shaders,
                               const QualityConfig& quality, const VolumeFrame& frame);

private:
    VolumeOutput recordImpl(const fx::ParticleRenderInputs* in, render::BufferRef external, uint32_t externalCount, render::RenderGraph& graph,
                            render::ShaderLibrary& shaders, const QualityConfig& quality, const VolumeFrame& frame);
    render::Device& m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload;  // ring of per-record constants (VolumeConstants)
    uint8_t* m_mapped = nullptr;
    uint32_t m_next = 0;
};
} // namespace unx::volume
