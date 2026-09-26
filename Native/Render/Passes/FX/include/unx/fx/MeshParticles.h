#pragma once
// Mesh particles (A3, FEATURES_GAME 0.A 7b; render C): the live particles of mesh programs (FX_OUTPUT_MESH, stream
// executor version 4 orientations) written as scene instances into C's GPU-written instance range
// (GpuScene::gpuInstanceRange, INTERFACES v1.58) by FxMeshInstances.hlsl, before V's culling. The frame calls
// tracks::particleMeshes after tracks::simulation (the tick is recorded) and before tracks::visibility.
//
// Mesh table: the host maps each program asset key (NV_StreamProgram mesh_asset, 64 bits) to a scene mesh index
// (UnxVfxMapMeshAsset; the render thread resolves runtime mesh ids) and sets fx::meshAssets(trackState) for the frame.
// A key without a mesh (never mapped, unmapped, or a removed runtime mesh) draws nothing and is counted.
#include "unx/render/Frame.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace unx::render
{
class Device;
struct FramePassContext;
}

namespace unx::fx
{
class ParticleSystem;

struct MeshAsset
{
    uint64_t asset = 0;          // the program's mesh_asset key
    uint32_t mesh = 0xFFFFFFFFu; // GPU scene mesh index (0xFFFFFFFF: none)
};

// Counters of one recorded frame (FxMeshInstances.hlsl counters[]).
struct MeshParticleStats
{
    uint32_t instances = 0;  // instances written
    uint32_t overflow = 0;   // live mesh particles past the range's capacity (fx.particles.mesh_instances_max; not drawn)
    uint32_t unmapped = 0;   // live mesh particles whose asset key has no mesh (not drawn)
    uint32_t status = 0;     // bit 0: a mesh program without orientation state (not drawn)
    uint32_t mode = 0;       // previous-transform source: 0 motion over the frame time, 1 same tick, 2 the previous frame's records
    bool recorded = false;   // the pass ran this frame
};

class MeshParticlePass
{
public:
    explicit MeshParticlePass(render::Device& device);
    ~MeshParticlePass();
    MeshParticlePass(const MeshParticlePass&) = delete;
    MeshParticlePass& operator=(const MeshParticlePass&) = delete;

    // Records the instance writer of this frame (nothing without a GPU instance range, a tick or orientation state).
    void record(ParticleSystem& particles, render::FramePassContext& fc, const std::vector<MeshAsset>& assets);
    // Counters of the last recorded frame (waits for the GPU; tests and statistics).
    MeshParticleStats stats();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The frame's mesh table (TrackState key "fx.meshAssets"): the host replaces it when its mapping changes.
std::vector<MeshAsset>& meshAssets(render::TrackState& state);
// The pass of a renderer (TrackState key "fx.meshes"), created on first use; null before.
MeshParticlePass* findMeshParticles(render::TrackState& state);
} // namespace unx::fx

namespace unx::render::tracks
{
// A3 mesh particles: after tracks::simulation, before every view's tracks::visibility.
void particleMeshes(FramePassContext& fc);
}
