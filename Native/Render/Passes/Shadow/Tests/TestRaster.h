#pragma once
// S test stand-ins for V (depth raster service, INTERFACES 5.3) and for V + M's main-view depth and G-buffer, drawing the
// scene's source triangles with TestRaster.hlsl. Correctness only; V's cluster pipeline replaces them in the renderer.
#include "../../Atmosphere/Tests/TestFrame.h"

#include <string>
#include <vector>

namespace unx::stest
{
struct TestView
{
    float4x4 viewProj;
    uint32_t userData, pad[3];
};
static_assert(sizeof(TestView) == 80);

class TestRaster
{
public:
    explicit TestRaster(TestFrame& tf) : m_tf(tf) {}

    // Installs the depth raster service into the test frame's FrameServices.
    void install()
    {
        m_tf.testServices.rasterizeDepth = [this](FramePassContext& fc, const DepthRasterRequest& r) { rasterizeDepth(fc, r); };
    }

    std::vector<uint4> chunks(uint32_t instanceMask) const
    {
        std::vector<uint4> out;
        const auto& inst = m_tf.gpuScene.instances();
        const auto& meshes = m_tf.gpuScene.meshes();
        for (uint32_t i = 0; i < inst.size(); ++i)
        {
            if ((inst[i].flags & instanceMask) == 0) continue;
            const uint32_t n = meshes[inst[i].mesh].triangleCount;
            for (uint32_t t = 0; t < n; t += 64) out.push_back({ i, t, std::min(64u, n - t), 0 });
        }
        return out;
    }

    void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& r)
    {
        if (r.depthTarget.valid() && r.atlasSlots.valid())
        {
            rasterizeAtlas(fc, r);
            return;
        }
        const std::vector<uint4> ch = chunks(r.instanceMask);
        if (ch.empty() || r.pixelKernel.empty()) return;
        std::vector<TestView> views;
        for (const RasterView& v : r.views) views.push_back({ v.viewProj, v.userData, { 0, 0, 0 } });
        const BufferRef chunkBuf = m_tf.uploadBuffer(fc, ch.data(), ch.size() * 16, 16, "test raster chunks");
        const BufferRef viewBuf = m_tf.uploadBuffer(fc, views.data(), views.size() * sizeof(TestView), sizeof(TestView), "test raster views");
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/Tests/TestRaster.GBUFFER0";
        d.pixelShader = r.pixelKernel;
        d.depthWrite = false;
        d.cull = r.cull;
        d.conservative = r.conservative;
        ID3D12PipelineState* pso = fc.shaders.mesh("s.test.raster|" + r.pixelKernel, d);
        const D3D12_GPU_VIRTUAL_ADDRESS constants = fc.frameConstantsFor(fc.frame.mainView);
        const DepthRasterRequest req = r;
        const uint32_t groups = (uint32_t)ch.size();
        fc.graph.addPass(r.name + ".test", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             for (const auto& [t, use] : req.textureUses) b.use(t, use);
                             for (const auto& [bf, use] : req.bufferUses) b.use(bf, use);
                             b.use(chunkBuf, Use::SrvGraphics);
                             b.use(viewBuf, Use::SrvGraphics);
                             b.keep();
                         },
                         [=](PassContext& c) {
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(constants);
                             c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                             for (uint32_t i = 0; i < (uint32_t)req.views.size(); ++i)
                             {
                                 const RasterView& v = req.views[i];
                                 uint32_t k[32] = {};
                                 k[0] = c.srv(chunkBuf);
                                 k[1] = c.srv(viewBuf);
                                 k[2] = i;
                                 k[3] = groups;
                                 for (int j = 0; j < 16; ++j) k[16 + j] = req.pixelConstants[j];
                                 c.graphicsConstants(k, 32);
                                 D3D12_VIEWPORT vp{ (float)v.viewportX, (float)v.viewportY, (float)v.viewportWidth, (float)v.viewportHeight, 0, 1 };
                                 D3D12_RECT sc{ (LONG)v.viewportX, (LONG)v.viewportY, (LONG)(v.viewportX + v.viewportWidth), (LONG)(v.viewportY + v.viewportHeight) };
                                 c.cmd->RSSetViewports(1, &vp);
                                 c.cmd->RSSetScissorRects(1, &sc);
                                 c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                             }
                         });
    }

    // Tile atlas mode (v1.32): every view over its whole viewport through TestAtlasPixel into a R32_UINT copy of the atlas
    // (one per atlas and frame, started from the requester's cleared atlas), then copied to the atlas after each request
    // (the copy accumulates the frame's requests, so later copies keep earlier requests' pages).
    void rasterizeAtlas(FramePassContext& fc, const DepthRasterRequest& r)
    {
        const std::vector<uint4> ch = chunks(r.instanceMask);
        const TextureRef atlas = r.depthTarget;
        const TextureDesc ad = fc.graph.desc(atlas);
        if (m_atlasFrame != fc.frame.frameIndex || m_atlasTarget != atlas.id)
        {
            m_atlasFrame = fc.frame.frameIndex;
            m_atlasTarget = atlas.id;
            m_atlasCopy = fc.graph.createTexture(TextureDesc{ "test atlas copy", ad.width, ad.height, 1, 1, DXGI_FORMAT_R32_UINT });
            const TextureRef copy = m_atlasCopy;
            fc.graph.addPass(r.name + ".test.init", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(atlas, Use::CopySrc);
                                 b.use(copy, Use::CopyDst);
                             },
                             [=](PassContext& c) { c.cmd->CopyResource(c.resource(copy), c.resource(atlas)); });
        }
        const TextureRef copy = m_atlasCopy;
        if (!ch.empty())
        {
            std::vector<TestView> views;
            for (const RasterView& v : r.views) views.push_back({ v.viewProj, v.userData, { 0, 0, 0 } });
            const BufferRef chunkBuf = m_tf.uploadBuffer(fc, ch.data(), ch.size() * 16, 16, "test atlas chunks");
            const BufferRef viewBuf = m_tf.uploadBuffer(fc, views.data(), views.size() * sizeof(TestView), sizeof(TestView), "test atlas views");
            MeshPipelineDesc d;
            d.meshShader = "Passes/Shadow/Tests/TestRaster.GBUFFER0";
            d.pixelShader = "Passes/Shadow/Tests/TestAtlasPixel";
            d.depthWrite = false;
            d.cull = r.cull;
            ID3D12PipelineState* pso = fc.shaders.mesh("s.test.atlas", d);
            const D3D12_GPU_VIRTUAL_ADDRESS constants = fc.frameConstantsFor(fc.frame.mainView);
            const DepthRasterRequest req = r;
            const uint32_t groups = (uint32_t)ch.size();
            fc.graph.addPass(r.name + ".test", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(copy, Use::UavGraphics);
                                 b.use(req.cullMask, Use::SrvGraphics);
                                 b.use(req.atlasSlots, Use::SrvGraphics);
                                 b.use(chunkBuf, Use::SrvGraphics);
                                 b.use(viewBuf, Use::SrvGraphics);
                             },
                             [=](PassContext& c) {
                                 c.cmd->SetPipelineState(pso);
                                 c.bindFrameConstants(constants);
                                 c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                                 for (uint32_t i = 0; i < (uint32_t)req.views.size(); ++i)
                                 {
                                     const RasterView& v = req.views[i];
                                     uint32_t k[32] = {};
                                     k[0] = c.srv(chunkBuf);
                                     k[1] = c.srv(viewBuf);
                                     k[2] = i;
                                     k[3] = groups;
                                     k[16] = c.uav(copy);
                                     k[17] = c.srv(req.cullMask);
                                     k[18] = c.srv(req.atlasSlots);
                                     k[19] = v.cullMaskOffset;
                                     k[20] = v.viewportWidth / req.cullTilePx;
                                     k[21] = req.atlasTilesPerRow;
                                     k[22] = req.cullTilePx;
                                     c.graphicsConstants(k, 32);
                                     D3D12_VIEWPORT vp{ 0, 0, (float)v.viewportWidth, (float)v.viewportHeight, 0, 1 };
                                     D3D12_RECT sc{ 0, 0, (LONG)v.viewportWidth, (LONG)v.viewportHeight };
                                     c.cmd->RSSetViewports(1, &vp);
                                     c.cmd->RSSetScissorRects(1, &sc);
                                     c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                                 }
                             });
        }
        fc.graph.addPass(r.name + ".test.copy", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(copy, Use::CopySrc);
                             b.use(atlas, Use::CopyDst);
                         },
                         [=](PassContext& c) { c.cmd->CopyResource(c.resource(atlas), c.resource(copy)); });
    }

    // Main-view depth (D32, reversed Z) and G-buffer (RG32_UINT) of every instance.
    void mainView(FramePassContext& fc, ViewResources& view)
    {
        const std::vector<uint4> ch = chunks(0xFFFFFFFFu);
        const uint32_t w = view.view.width, h = view.view.height;
        view.depth = fc.graph.createTexture(TextureDesc{ "test depth", w, h, 1, 1, DXGI_FORMAT_D32_FLOAT });
        view.gbuffer = fc.graph.createTexture(TextureDesc{ "test gbuffer", w, h, 1, 1, DXGI_FORMAT_R32G32_UINT });
        TestView tv{ view.view.viewProj, 0, { 0, 0, 0 } };
        const BufferRef chunkBuf = m_tf.uploadBuffer(fc, ch.data(), ch.size() * 16, 16, "test gbuffer chunks");
        const BufferRef viewBuf = m_tf.uploadBuffer(fc, &tv, sizeof tv, sizeof tv, "test gbuffer view");
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/Tests/TestRaster.GBUFFER1";
        d.pixelShader = "Passes/Shadow/Tests/TestGBuffer";
        d.renderTargets = { DXGI_FORMAT_R32G32_UINT };
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.cull = D3D12_CULL_MODE_BACK;
        ID3D12PipelineState* pso = fc.shaders.mesh("s.test.gbuffer", d);
        const TextureRef depth = view.depth, gbuffer = view.gbuffer;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
        const uint32_t groups = (uint32_t)ch.size();
        fc.graph.addPass("s.test.gbuffer", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(depth, Use::DepthWrite);
                             b.use(gbuffer, Use::RenderTarget);
                             b.use(chunkBuf, Use::SrvGraphics);
                             b.use(viewBuf, Use::SrvGraphics);
                         },
                         [=](PassContext& c) {
                             const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(gbuffer), dsv = c.dsv(depth);
                             const float zero[4] = {};
                             c.cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                             c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
                             c.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(constants);
                             uint32_t k[4] = { c.srv(chunkBuf), c.srv(viewBuf), 0, groups };
                             c.graphicsConstants(k, 4);
                             D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
                             D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
                             c.cmd->RSSetViewports(1, &vp);
                             c.cmd->RSSetScissorRects(1, &sc);
                             c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                         });
    }

private:
    TestFrame& m_tf;
    uint64_t m_atlasFrame = UINT64_MAX;
    uint32_t m_atlasTarget = UINT32_MAX;
    TextureRef m_atlasCopy;
};

// Axis-aligned box mesh (outward normals, counter-clockwise front faces) with half extents e around the origin.
inline scene::Mesh boxMesh(const char* name, float3 e)
{
    scene::Mesh m;
    m.name = name;
    const float3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const float3& f : n)
    {
        // Face basis: u, v with u x v = f.
        const float3 u = std::abs(f.y) > 0.5f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
        const float3 v = cross(f, u);
        const float3 uu = cross(v, f);
        const uint32_t base = (uint32_t)m.positions.size();
        const float s[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (auto& c : s)
        {
            const float3 p = f + uu * c[0] + v * c[1];
            m.positions.push_back(p * e);
            m.normals.push_back(f);
            m.uv0.push_back({ (c[0] + 1) * 0.5f, (c[1] + 1) * 0.5f });
        }
        for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) m.indices.push_back(base + i);
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

inline scene::Instance instanceAt(uint32_t mesh, float3 position, uint32_t flags = scene::InstanceCastShadow)
{
    scene::Instance i;
    i.mesh = mesh;
    i.transform = float3x4::translation(position);
    i.flags = flags;
    return i;
}
} // namespace unx::stest
