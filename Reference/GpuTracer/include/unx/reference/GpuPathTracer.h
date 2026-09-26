#pragma once
// GPU reference path tracer (Reference/GpuTracer/README_KO.md). The estimator of unx::reference::PathTracer on standard
// D3D12 (DXR 1.1 inline ray queries in compute shaders, SM 6.6 bindless, double-precision adds for the accumulation):
// the same sampler, material, atmosphere and light code (Reference/GpuTracer/shared, compiled for both CPU tests and
// GPU), the same path structure, MIS, forced in-scattering, Russian roulette and sun-caustic light tracing, over the
// scene's original geometry (its own BLAS/TLAS; alpha-tested leaves resolved exactly on non-opaque candidates).
// Coexistence: dispatches of about kTargetDispatchMs, structurally at most 2^19 paths of at most 16 samples per thread
// each (GpuPathTracer.cpp kMaxPathsPerDispatch), the GPU measurement lock held in <= 15 s slices (kind correctness),
// pauses while .gpulock/HOLD exists. The tracer takes the lock itself: run it directly, not under a GpuLock.ps1
// wrapper (under one it detects the wrapper as its ancestor and works inside that lock, without slices, so other
// sessions wait for the whole run).
#include "unx/reference/PathTracer.h"

#include <filesystem>
#include <memory>
#include <string>

struct ID3D12Resource;
namespace unx::render
{
class Device;
}

namespace unx::reference
{
struct GpuRenderInfo
{
    std::string adapter, driver;
    uint64_t vramSceneBytes = 0;      // buffers + acceleration structures + accumulators (allocated by the tracer)
    uint64_t vramProcessPeakBytes = 0;  // DXGI process usage (local segment), peak seen
    double buildSeconds = 0;          // scene upload + BLAS/TLAS
    double gpuSeconds = 0;            // wall time inside slices (GPU busy with this job)
    double lockWaitSeconds = 0;       // waiting for the lock or HOLD
    uint32_t slices = 0;
    double longestSliceSeconds = 0;
    uint32_t dispatches = 0;
    double longestDispatchMs = 0;     // GPU timestamps
    double meanDispatchMs = 0;
    uint32_t errors = 0;              // kRtError* bits (non-zero: the render failed)
    uint32_t maxCandidatesPerRay = 0; // most non-opaque candidates one ray visited (cap kRtMaxCandidates = 4096)
};

// Shutter time integral (photo and cinematic mode, FEATURES_GAME 17; README 6). The scene (instance transforms,
// skeleton poses) and the camera given to start() are the state at shutter open (scene time `open`: wind); this gives the
// state at close. Each pixel then integrates over the shutter: every epoch renders one half at its own time sample
// t = open + (close - open) u (u stratified per half), with the game's tick interpolation rule - positions and scales
// linear, rotations slerp - for the instances, the skeletons' joints (model space) and the camera.
struct ShutterMotion
{
    float open = 0, close = 0;                   // scene time (s)
    float3 cameraPosition, cameraForward, cameraUp;  // at close (used when close > open)
    std::vector<float3x4> instances;             // per scene instance, object -> world at close; empty: none moves
    std::vector<std::vector<float3x4>> skeletons;  // per skeleton, jointToModel at close; empty: poses fixed
};

class GpuPathTracer
{
public:
    static constexpr double kTargetDispatchMs = 25.0;

    // repoRoot: for .gpulock (slices) and the kernels next to the executable (bin/shaders/Reference). An empty root
    // (photo mode inside a game) runs without the measurement-lock protocol.
    GpuPathTracer(const scene::Scene& scene, std::filesystem::path repoRoot, std::string what);
    // Photo mode in a game (render A request): the renderer's device (no second device in the process: the VRAM budget
    // of an 8 GB PC; the result stays on the device for the post chain). No measurement-lock slices. shaderDirectory: the
    // folder holding the tracer's kernels (bin/shaders/Reference of a build, deployed beside the host plugin). The tracer
    // frees every descriptor it allocated when destroyed, after the device's queues are idle. Call it from the thread
    // that submits to the device (the render thread).
    GpuPathTracer(const scene::Scene& scene, render::Device& device, std::filesystem::path shaderDirectory, std::string what);
    ~GpuPathTracer();
    GpuPathTracer(const GpuPathTracer&) = delete;
    GpuPathTracer& operator=(const GpuPathTracer&) = delete;

    using Progress = std::function<void(const RenderStats&)>;
    // Same settings and output as PathTracer::render (checkpoints use their own file format).
    RenderOutput render(const ResolvedCamera& camera, const RenderSettings& settings, const Progress& progress = {});
    const GpuRenderInfo& info() const;

    // Progressive rendering (photo and cinematic mode, FEATURES_GAME 17): render() is start, passes until
    // settings.samplesPerPixel, then the output; the caller drives the same steps. Samples are those of render()
    // whatever the pass sizes (each sample index has its own sampler state), so stopping at n samples gives the image
    // render() gives for n, up to float rounding (a pass sums its samples in float before the double accumulation).
    // settings.samplesPerPixel is the target (the accumulation stops there).
    void start(const ResolvedCamera& camera, const RenderSettings& settings);
    // Before start() or render(): the shutter's motion (a default ShutterMotion: none - the static estimator, unchanged).
    void setMotion(const ShutterMotion& motion);
    // Before the first start() or render(), on the tracer's own device: build it on WARP (software rasteriser) instead of
    // the GPU - correctness runs that cannot hang the GPU (no measurement-lock slices; timings are the CPU's).
    void setWarp(bool warp);
    // One pass: at most maxHalfSamples samples per pixel and half (the pass is also sized to about kTargetDispatchMs
    // per dispatch). Returns the samples per pixel done, both halves together.
    uint32_t pass(uint32_t maxHalfSamples = UINT32_MAX);
    uint32_t samplesDone() const;
    // The accumulation so far: image = mean of both halves (radiance x exposure), halvesRelMse between the halves.
    // The image's own relMSE against the converged image is about halvesRelMse / 4 (two independent halves).
    RenderOutput current();
    // The same image on the GPU, without a read-back (photo mode's progressive display, render A request): mean of the
    // halves x exposure (current().image's operations and order), width x height DXGI_FORMAT_R32G32B32A32_FLOAT, alpha 1,
    // in the COMMON layout (D3D12_BARRIER_LAYOUT_COMMON), complete when this returns (the tracer's queue waited). Updated
    // on each call once samples were added; the revision is samplesDone() at that update. Owned by the tracer.
    ID3D12Resource* currentImageResource();
    uint32_t currentImageRevision() const;
    // halvesRelMse of that update, reduced on the GPU with it (render A request: progress without a read-back of the
    // halves): metrics::relMse(halfA, halfB) on the same float halves, summed in a fixed order (8x8 groups, then one group
    // of 1024), so it differs from current().halvesRelMse only by the order of the double additions. 0 before the first
    // update. Costs one extra dispatch and an 8-byte read-back per update.
    double currentHalvesRelMse() const;

    // The atmosphere's tau_top table is built on the GPU (AtmosphereTable.hlsl); this builds entries [first, first + count)
    // and compares each with the CPU's AtmosphereModel::tableEntry: channels that differ (bitwise) and the largest
    // relative difference. Tests only.
    struct AtmosphereTableCheck
    {
        uint32_t entries = 0, differing = 0;
        double maxRelative = 0, seconds = 0;  // seconds: the GPU build of the entries
    };
    AtmosphereTableCheck checkAtmosphereTable(uint32_t first, uint32_t count);
    // Diagnostics (unx_reference probe --device gpu): PathTracer::primaryIdentities on the GPU for the pixel rectangle
    // [x0, x0 + columns) x [y0, y0 + rows) of a width x height image: 17 identities per pixel (4 x 4 stratum centres,
    // then the centre), row-major over the rectangle; (instance << 32 | mesh triangle) or kSkyIdentity.
    std::vector<uint64_t> primaryIdentities(const ResolvedCamera& camera, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint32_t columns,
                                            uint32_t rows);

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};
} // namespace unx::reference
