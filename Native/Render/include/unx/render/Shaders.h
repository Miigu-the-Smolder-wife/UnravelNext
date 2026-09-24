#pragma once
#include "unx/render/Device.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace unx::render
{
struct MeshPipelineDesc
{
    std::string meshShader;   // kernel name, e.g. "Test/Triangle.ms"
    std::string pixelShader;  // empty = depth-only
    std::vector<DXGI_FORMAT> renderTargets;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    bool depthWrite = true;
    D3D12_COMPARISON_FUNC depthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
    D3D12_CULL_MODE cull = D3D12_CULL_MODE_BACK;
    bool frontCounterClockwise = true;  // false for mirrored (planar reflection) views
    bool conservative = false;
};

struct PipelineStats
{
    uint32_t created = 0;
    double totalCpuMs = 0;  // sum over pipelines
    double wallMs = 0;      // parallel wall time of the last createAll
    double slowestMs = 0;
    std::string slowest;
};

// Loads build-time DXIL (Tools/ShaderCompiler, one kernel per file) from <exe>/shaders and creates pipelines on the
// shared job pool. Kernel names are paths relative to Native/Render/Passes without ".hlsl", plus variant suffixes
// (".MODE1"). Every pipeline uses the device's bindless root signature.
class ShaderLibrary
{
public:
    ShaderLibrary(Device& device, std::filesystem::path directory);

    ID3D12PipelineState* compute(const std::string& kernel);
    ID3D12PipelineState* mesh(const std::string& name, const MeshPipelineDesc& desc);

    // Creates the given compute kernels in parallel (cold start path). Returns wall time.
    void createAll(const std::vector<std::string>& computeKernels);
    const PipelineStats& stats() const { return m_stats; }
    const std::filesystem::path& directory() const { return m_directory; }

private:
    std::vector<uint8_t> load(const std::string& kernel) const;
    ComPtr<ID3D12PipelineState> createCompute(const std::string& kernel);
    Device& m_device;
    std::filesystem::path m_directory;
    std::mutex m_mutex;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> m_pipelines;
    PipelineStats m_stats;
};
} // namespace unx::render
