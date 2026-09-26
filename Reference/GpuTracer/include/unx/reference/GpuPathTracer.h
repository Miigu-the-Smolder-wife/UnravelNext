#pragma once
// GPU reference path tracer (Reference/GpuTracer/README_KO.md). The estimator of unx::reference::PathTracer on standard
// D3D12 (DXR 1.1 inline ray queries in compute shaders, SM 6.6 bindless, double-precision adds for the accumulation):
// the same sampler, material, atmosphere and light code (Reference/GpuTracer/shared, compiled for both CPU tests and
// GPU), the same path structure, MIS, forced in-scattering, Russian roulette and sun-caustic light tracing, over the
// scene's original geometry (its own BLAS/TLAS; alpha-tested leaves resolved exactly on non-opaque candidates).
// Coexistence: dispatches of about kTargetDispatchMs, the GPU measurement lock held in <= 15 s slices (kind
// correctness), pauses while .gpulock/HOLD exists.
#include "unx/reference/PathTracer.h"

#include <filesystem>
#include <memory>
#include <string>

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
};

class GpuPathTracer
{
public:
    static constexpr double kTargetDispatchMs = 25.0;

    // repoRoot: for .gpulock (slices) and the kernels next to the executable (bin/shaders/Reference).
    GpuPathTracer(const scene::Scene& scene, std::filesystem::path repoRoot, std::string what);
    ~GpuPathTracer();
    GpuPathTracer(const GpuPathTracer&) = delete;
    GpuPathTracer& operator=(const GpuPathTracer&) = delete;

    using Progress = std::function<void(const RenderStats&)>;
    // Same settings and output as PathTracer::render (checkpoints use their own file format).
    RenderOutput render(const ResolvedCamera& camera, const RenderSettings& settings, const Progress& progress = {});
    const GpuRenderInfo& info() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};
} // namespace unx::reference
