#pragma once
#include "unx/render/Frame.h"
#include "unx/render/Shaders.h"
#include <string>

namespace unx::render
{
inline void preserveHistoryResize(FramePassContext& fc, ComPtr<ID3D12Resource> old, ID3D12Resource* next,
                                  uint32_t block = 1, uint32_t oldRows = 0, uint32_t nextRows = 0)
{
    if (!old || !next) return;
    const auto a = old->GetDesc(), b = next->GetDesc();
    if (a.Format != b.Format || a.Dimension != b.Dimension) fail("history resize: incompatible formats or dimensions");
    uint32_t type;
    switch (b.Format)
    {
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R32_FLOAT: type = 0; break;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: type = 1; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: type = 2; break;
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R32_UINT: type = 3; break;
    case DXGI_FORMAT_R32G32_UINT: type = 4; break;
    default: fail("history resize: unsupported format %u", (uint32_t)b.Format);
    }
    const bool volume = b.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const TextureRef src = fc.graph.importTexture(old.Get(), {"history resize source", (uint32_t)a.Width, a.Height, a.DepthOrArraySize, 1, a.Format, a.Dimension},
                                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef dst = fc.graph.importTexture(next, {"history resize destination", (uint32_t)b.Width, b.Height, b.DepthOrArraySize, 1, b.Format, b.Dimension},
                                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const uint32_t width = (uint32_t)b.Width, height = b.Height, depth = volume ? b.DepthOrArraySize : 1;
    const uint32_t oldColumns = (uint32_t)a.Width / block, nextColumns = width / block;
    if (!oldRows) oldRows = a.Height / block;
    if (!nextRows) nextRows = height / block;
    Device* device = &fc.device;
    ShaderLibrary* shaders = &fc.shaders;
    const std::string kernel = "Passes/Common/HistoryResize.TYPE" + std::to_string(type) + ".DIM" + (volume ? "3" : "2");
    fc.graph.addPass("history.resize", QueueType::Compute,
        [&](PassBuilder& pass) {
            pass.use(src, Use::SrvCompute); pass.use(dst, Use::UavCompute);
            // Keep the old resource until the queue has signalled the resample itself, then retire it.
            pass.fenceAfter([old, device](Queue&, uint64_t) { device->deferRelease(old); });
        },
        [=](PassContext& c) {
            const uint32_t k[12] = {c.srv(src), c.uav(dst), block, 0, width, height, depth, 0, oldColumns, oldRows, nextColumns, nextRows};
            c.cmd->SetPipelineState(shaders->compute(kernel));
            c.computeConstants(k, 12);
            c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, depth);
        });
}
}
