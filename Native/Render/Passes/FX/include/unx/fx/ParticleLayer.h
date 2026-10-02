#pragma once
// Particle render pass (G7 of ARCHITECTURE 4.1; request 20260926_FX_particle_render_pass.md): the particle layer of a
// view from the particle module's latest tick, interpolated to the frame time. Per frame: the particles at the frame time
// -> records -> tile lists -> per tile a depth sort, the 1/4-resolution composite and the full-resolution edge blocks
// (ParticleLayer.hlsli: what M's shading composite reads). Sprites, emission only for now (M0 stage 1).
#include "unx/fx/Particles.h"
#include "unx/render/Frame.h"

#include <cstdint>
#include <wrl/client.h>

namespace unx::fx
{
// Stage 2 inputs (request 20260926_FX_particle_render_pass 3): what lit particles and the air in front of every particle
// read, from the frame's and the view's resources (invalid = absent).
struct ParticleLighting
{
    render::BufferRef vsmPageTable, vsmPool, vsmBlocks, vsmSearchBound, vsmLayers, froxelLights;
    render::GiSource gi;  // the indirect light (unx/render/Frame.h: the translucency volume or the world GI cache)
    render::BufferRef fxLights;  // v1.81: the scene light buffer with the FX tail (declared so the FX writer comes first)
    render::TextureRef vsmAtlas, airVolume, transmittanceLut, multiScatterLut;
    render::TextureRef fogVolume;  // atmosphere.fog (FrameResources): read with the air (declared beside it)
    // shading.mega_lights: the froxel grid's sampled local light (FrameResources::localFluence / localMoment; invalid: the
    // setup loops over the froxel list's lights with S's shadow maps)
    render::TextureRef localFluence, localMoment;
    uint32_t vsmConstants = 0xFFFFFFFFu, vsmLocalLights = 0xFFFFFFFFu, vsmSlotOfLight = 0xFFFFFFFFu;
};

struct ParticleLayerFrame
{
    const render::ViewDesc* view = nullptr;        // size, matrices
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;  // the view's frame constants (root CBV b1 of the pass's kernels)
    render::TextureRef depth;                      // the view's opaque depth (D32, reversed Z)
    double camera[3] = {};                         // the camera's position in the renderer's world (frame + worldOrigin; double:
                                                   // the anchor offset is a difference)
    float streamAxes[3] = { 1, 1, 1 };             // stream space -> renderer world axis signs (FrameContext::streamAxes)
    double time = 0;                               // the particle stream's context time of this frame (s)
    bool soft = false, nearFade = false;           // fx.particles.soft / near_fade (FxLayerTile.hlsl): a sprite as a ball
                                                   // that surfaces cut softly, and that fades out at the near plane
    ParticleLighting lighting;
};

struct ParticleLayerOutput
{
    bool valid = false;  // false: no tick yet (nothing to draw; the view's particle fields stay invalid)
    render::TextureRef layer, depthRange;  // RGBA16F / RG16F, ceil(W/4) x ceil(H/4)
    render::BufferRef edges;               // raw: count, edge index table, edge blocks (ParticleLayer.hlsli)
    // the pass's own resources (tests read them)
    render::BufferRef constants, records, tileCounts, tileStarts, entries, counters;
    render::BufferRef ribbonVertices, ribbonAppearance;  // this frame's strips (the records' sample function reads them)
    uint32_t threads = 0, tiles = 0, layerWidth = 0, layerHeight = 0, entryCapacity = 0, edgeCapacity = 0;
    uint32_t recordCount = 0;  // records: threads sprite records, then one per ribbon point (strip segments)
    float w = 0;  // frame time between the previous tick's end (0) and the latest tick's end (1)
};

class ParticleLayerPass
{
public:
    explicit ParticleLayerPass(render::Device& device);
    ~ParticleLayerPass();
    ParticleLayerPass(const ParticleLayerPass&) = delete;
    ParticleLayerPass& operator=(const ParticleLayerPass&) = delete;

    // Records the pass into 'graph' (frame 'importIndex').
    ParticleLayerOutput record(ParticleSystem& particles, render::RenderGraph& graph, render::ShaderLibrary& shaders, uint64_t importIndex,
                               const ParticleLayerFrame& frame);

private:
    render::Device& m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload;  // ring of per-record constants (LayerConstants)
    uint8_t* m_mapped = nullptr;
    uint32_t m_next = 0;
};
} // namespace unx::fx
