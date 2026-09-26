// S performance gate (GPU lock required): per-pass GPU time of the S track's passes at 4K / 1440p on a C-track scene.
//   unx_gate_shadow_shadowgate --scene FILE.unxscene [--camera N] [--select-radius M] [--resolution 4K|1440p|both]
//                              [--frames N] [--atmosphere-every-frame] [--out DIR] [--set key=value] [--dump-paths DIR]
// V's cluster pipeline is not connected yet, so the main view (depth + G-buffer) and the shadow pages are drawn once by
// the S test raster (source triangles, no LOD) during warm-up; the measured frames are the steady state of a static
// camera: page requests, cache maintenance, visibility pass (and the atmosphere when forced to rebuild every frame).
// --select-radius keeps only instances within M metres of the camera for that one-off raster (scenes whose instanced
// triangle count needs V's culling); the report states it.
#include "../Tests/TestRaster.h"

#include "VsmSystem.h"

#include "unx/core/File.h"
#include "unx/render/GpuLock.h"
#include "unx/render/Harness.h"

#include <cstdio>
#include <fstream>

using namespace unx;
using namespace unx::render;

namespace
{
// Persistent upload-heap buffer with a raw SRV (read by the test raster; no GPU writes, so not declared in the graph).
struct UploadBuffer
{
    ComPtr<ID3D12Resource> resource;
    uint32_t srv = UINT32_MAX;
    uint8_t* mapped = nullptr;

    void create(Device& d, uint64_t bytes, uint32_t stride)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&resource)), "gate upload");
        D3D12_RANGE none{ 0, 0 };
        check(resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map gate upload");
        srv = d.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.NumElements = (UINT)(bytes / stride);
        sd.Buffer.StructureByteStride = stride;
        d.d3d()->CreateShaderResourceView(resource.Get(), &sd, d.descriptors().resourceCpu(srv));
    }
};

struct Gate
{
    Device& device;
    ShaderLibrary& shaders;
    QualityConfig& quality;
    GpuScene& scene;
    TrackState state;
    FrameContext frame;
    UploadBuffer constants;  // frame constants ring: 64 slots of 1 KB
    uint32_t slot = 0;
    UploadBuffer chunksAll, chunksShadow, views;
    uint32_t chunkCountAll = 0, chunkCountShadow = 0;
    ComPtr<ID3D12Resource> depth, gbuffer;
    uint32_t width = 0, height = 0;

    D3D12_GPU_VIRTUAL_ADDRESS frameConstants(const ViewDesc& view)
    {
        gpu::FrameConstants c{};
        c.viewProj = view.viewProj;
        c.prevViewProj = view.prevViewProj;
        c.invViewProj = view.invViewProj;
        c.view = view.view;
        c.proj = view.proj;
        c.cameraPosition = view.position;
        c.nearPlane = view.nearPlane;
        c.clipPlane = view.clipPlane;
        c.viewWidth = view.width;
        c.viewHeight = view.height;
        c.viewKind = (uint32_t)view.kind;
        c.frameIndex = (uint32_t)frame.frameIndex;
        c.time = (float)frame.time;
        c.deltaTime = frame.deltaTime;
        c.exposure = 1.0f / (1.2f * std::exp2(view.ev100));
        c.tanHalfFovY = std::tan(view.verticalFov * 0.5f);
        const scene::Scene& s = *scene.source();
        c.sunDirection = s.sun.direction;
        c.sunIlluminance = s.sun.illuminance;
        c.sunColor = s.sun.color;
        c.sunAngularRadius = s.sun.angularRadius;
        c.windDirection = s.windDirection;
        c.windSpeed = s.windSpeed;
        scene.fill(c);
        const uint32_t at = (slot++ % 64) * 1024;
        std::memcpy(constants.mapped + at, &c, sizeof c);
        return constants.resource->GetGPUVirtualAddress() + at;
    }

    FramePassContext context(RenderGraph& graph, FrameResources& resources, FrameServices& services)
    {
        return FramePassContext{ device, graph, shaders, quality, scene, frame, resources, services, [this](const ViewDesc& v) { return frameConstants(v); }, &state };
    }

    void buildChunks(const scene::Camera& cam, float radius)
    {
        std::vector<uint4> all, shadow;
        const auto& inst = scene.instances();
        const auto& meshes = scene.meshes();
        for (uint32_t i = 0; i < inst.size(); ++i)
        {
            const gpu::Instance& g = inst[i];
            const gpu::Mesh& m = meshes[g.mesh];
            if (radius > 0)
            {
                const float3 c{ g.objectToWorld[0].w, g.objectToWorld[1].w, g.objectToWorld[2].w };
                const float scale = length(float3{ g.objectToWorld[0].x, g.objectToWorld[0].y, g.objectToWorld[0].z });
                if (length(c - cam.position) - m.boundsSphere.w * scale > radius) continue;
            }
            for (uint32_t t = 0; t < m.triangleCount; t += 64)
            {
                const uint4 ch{ i, t, std::min(64u, m.triangleCount - t), 0 };
                all.push_back(ch);
                if (g.flags & scene::InstanceCastShadow) shadow.push_back(ch);
            }
        }
        chunkCountAll = (uint32_t)all.size();
        chunkCountShadow = (uint32_t)shadow.size();
        chunksAll.create(device, std::max<size_t>(all.size(), 1) * 16, 16);
        chunksShadow.create(device, std::max<size_t>(shadow.size(), 1) * 16, 16);
        std::memcpy(chunksAll.mapped, all.data(), all.size() * 16);
        std::memcpy(chunksShadow.mapped, shadow.data(), shadow.size() * 16);
        views.create(device, 64 * sizeof(stest::TestView), sizeof(stest::TestView));
        logf("gate raster: %u chunks (%u shadow casters) = %.1f M / %.1f M triangles\n", chunkCountAll, chunkCountShadow, chunkCountAll * 64 / 1e6, chunkCountShadow * 64 / 1e6);
    }

    void createTargets(uint32_t w, uint32_t h)
    {
        width = w;
        height = h;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dc{};
        dc.Format = DXGI_FORMAT_D32_FLOAT;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &dc, nullptr, 0, nullptr, IID_PPV_ARGS(&depth)), "gate depth");
        d.Format = DXGI_FORMAT_R32G32_UINT;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE gc{};
        gc.Format = DXGI_FORMAT_R32G32_UINT;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &gc, nullptr, 0, nullptr, IID_PPV_ARGS(&gbuffer)), "gate gbuffer");
    }

    void importTargets(RenderGraph& g, ViewResources& view)
    {
        view.depth = g.importTexture(depth.Get(), TextureDesc{ "gate depth", width, height, 1, 1, DXGI_FORMAT_D32_FLOAT }, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
        view.gbuffer = g.importTexture(gbuffer.Get(), TextureDesc{ "gate gbuffer", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT }, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    }

    // Main view into the persistent targets (warm-up only).
    void drawMain(FramePassContext& fc, const ViewResources& view)
    {
        stest::TestView tv{ view.view.viewProj, 0, { 0, 0, 0 } };
        std::memcpy(views.mapped, &tv, sizeof tv);
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/Tests/TestRaster.GBUFFER1";
        d.pixelShader = "Passes/Shadow/Tests/TestGBuffer";
        d.renderTargets = { DXGI_FORMAT_R32G32_UINT };
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.cull = D3D12_CULL_MODE_BACK;
        ID3D12PipelineState* pso = fc.shaders.mesh("s.gate.gbuffer", d);
        const TextureRef dt = view.depth, gt = view.gbuffer;
        const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
        const uint32_t chunks = chunksAll.srv, vs = views.srv, groups = chunkCountAll, w = width, h = height;
        fc.graph.addPass("s.gate.gbuffer", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(dt, Use::DepthWrite);
                             b.use(gt, Use::RenderTarget);
                             b.keep();
                         },
                         [=](PassContext& c) {
                             const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(gt), dsv = c.dsv(dt);
                             const float zero[4] = {};
                             c.cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                             c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
                             c.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             const uint32_t k[4] = { chunks, vs, 0, groups };
                             c.graphicsConstants(k, 4);
                             D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
                             D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
                             c.cmd->RSSetViewports(1, &vp);
                             c.cmd->RSSetScissorRects(1, &sc);
                             c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                         });
    }

    // Depth raster service for the warm-up frames (S pages).
    void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& r)
    {
        // The one-path VSM draws into V's tile atlas (hardware depth); this stand-in has only the pixel-kernel mode.
        if (r.depthTarget.valid() || r.pixelKernel.empty())
            fail("ShadowGate: request '%s' uses the depth atlas; measure the VSM with RendererGate (V's raster service)", r.name.c_str());
        for (size_t i = 0; i < r.views.size(); ++i)
        {
            stest::TestView tv{ r.views[i].viewProj, r.views[i].userData, { 0, 0, 0 } };
            std::memcpy(views.mapped + (1 + i) * sizeof tv, &tv, sizeof tv);
        }
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/Tests/TestRaster.GBUFFER0";
        d.pixelShader = r.pixelKernel;
        d.depthWrite = false;
        d.cull = r.cull;
        ID3D12PipelineState* pso = fc.shaders.mesh("s.gate.raster|" + r.pixelKernel, d);
        const D3D12_GPU_VIRTUAL_ADDRESS cb = fc.frameConstantsFor(fc.frame.mainView);
        const DepthRasterRequest req = r;
        const uint32_t chunks = chunksShadow.srv, vs = views.srv, groups = chunkCountShadow;
        fc.graph.addPass(r.name + ".gate", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             for (const auto& [t, use] : req.textureUses) b.use(t, use);
                             for (const auto& [bf, use] : req.bufferUses) b.use(bf, use);
                             b.keep();
                         },
                         [=](PassContext& c) {
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                             for (uint32_t i = 0; i < (uint32_t)req.views.size(); ++i)
                             {
                                 const RasterView& v = req.views[i];
                                 uint32_t k[32] = {};
                                 k[0] = chunks;
                                 k[1] = vs;
                                 k[2] = 1 + i;
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
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath, resolutions = "both", out, qualityPath = std::string(UNX_SOURCE_DIR) + "/Config/quality";
        uint32_t cameraIndex = 0, frames = 600;
        float selectRadius = 0;
        bool atmosphereEveryFrame = false;
        std::string dumpPaths;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--scene") scenePath = next();
            else if (a == "--camera") cameraIndex = (uint32_t)std::stoul(next());
            else if (a == "--select-radius") selectRadius = std::stof(next());
            else if (a == "--resolution") resolutions = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--atmosphere-every-frame") atmosphereEveryFrame = true;
            else if (a == "--out") out = next();
            else if (a == "--dump-paths") dumpPaths = next();  // DIR: per-pixel visibility path image after the warm-up
            else if (a == "--set") overrides.push_back(next());
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene FILE.unxscene is required (Tools/SceneGen)");
        requireGpuLock("unx_gate_shadow_shadowgate");
        QualityConfig quality = QualityConfig::loadDirectory(qualityPath);
        for (const std::string& o : overrides) quality.applyOverride(o);
        Device device(DeviceOptions{});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        const scene::Scene sc = scene::load(scenePath);
        GpuScene gpuScene(device);
        gpuScene.upload(sc);
        if (cameraIndex >= sc.cameras.size()) fail("camera %u not in the scene (%zu cameras)", cameraIndex, sc.cameras.size());
        const scene::Camera cam = sc.cameras[cameraIndex];
        logf("scene %s (%s), camera %u '%s', select radius %s\n", sc.name.c_str(), scene::contentHash(sc).substr(0, 16).c_str(), cameraIndex, cam.name.c_str(),
             selectRadius > 0 ? format("%.0f m", selectRadius).c_str() : "all");

        Harness harness(device, quality);
        std::vector<std::string> resList = resolutions == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutions };
        for (const std::string& rn : resList)
        {
            const Resolution res = resolutionFromString(rn, quality);
            Gate gate{ device, shaders, quality, gpuScene };
            gate.constants.create(device, 64 * 1024, 4);
            gate.buildChunks(cam, selectRadius);
            gate.createTargets(res.width, res.height);
            gate.frame.deltaTime = 1.0f / 165;
            auto mainView = [&](uint64_t f) {
                scene::Camera c = cam;
                if (atmosphereEveryFrame) c.position.y += (f & 1) ? 1e-3f : 0.0f;  // sky/aerial inputs change, pages do not
                ViewDesc v = ViewDesc::fromCamera(c, res.width, res.height, float4x4{});
                v.prevViewProj = v.viewProj;
                return v;
            };

            // Warm-up: main view once, pages until nothing is dirty.
            {
                RenderGraph graph(device);
                for (uint64_t f = 0; f < 8; ++f)
                {
                    gate.frame.frameIndex = f;
                    gate.frame.mainView = mainView(f);
                    FrameResources resources;
                    FrameServices services;
                    services.rasterizeDepth = [&](FramePassContext& c, const DepthRasterRequest& r) { gate.rasterizeDepth(c, r); };
                    FramePassContext fc = gate.context(graph, resources, services);
                    ViewResources view;
                    view.view = gate.frame.mainView;
                    view.frameConstants = fc.frameConstantsFor(view.view);
                    gate.importTargets(graph, view);
                    if (f == 0) gate.drawMain(fc, view);
                    tracks::atmosphere(fc);
                    tracks::shadowPages(fc, view);
                    shadow::setDebugPaths(gate.state, f == 7 && !dumpPaths.empty());
                    tracks::shadowVisibility(fc, view);
                    ComPtr<ID3D12Resource> rb;
                    if (f == 7 && !dumpPaths.empty())
                    {
                        const TextureRef vis = view.shadowVisibility;
                        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                        D3D12_RESOURCE_DESC1 rd{};
                        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        rd.Width = (uint64_t)((res.width * 4 + 255) & ~255u) * res.height;
                        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                        rd.SampleDesc.Count = 1;
                        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        rb.Reset();
                        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "paths readback");
                        ID3D12Resource* dst = rb.Get();
                        const uint32_t w = res.width, h = res.height;
                        graph.addPass("s.gate.paths", QueueType::Graphics, [&](PassBuilder& b) { b.use(vis, Use::CopySrc); b.keep(); },
                                      [=](PassContext& c) {
                                          D3D12_TEXTURE_COPY_LOCATION d{ dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                                          d.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, w, h, 1, (w * 4 + 255) & ~255u };
                                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(vis), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                          c.cmd->CopyTextureRegion(&d, 0, 0, 0, &src, nullptr);
                                      });
                    }
                    graph.execute(nullptr);
                    device.waitIdle();
                    gate.frame.time += gate.frame.deltaTime;
                    if (rb)
                    {
                        // Colours: sky black, no caster white, reach lit light green, reach umbra dark blue, search lit green,
                        // filtered red, disk lit yellow, disk umbra blue.
                        static const uint8_t colours[8][3] = { { 255, 255, 255 }, { 160, 255, 160 }, { 0, 0, 120 }, { 0, 190, 0 }, { 255, 0, 0 }, { 255, 255, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };
                        uint8_t* p = nullptr;
                        check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map paths");
                        const uint32_t pitch = (res.width * 4 + 255) & ~255u;
                        std::vector<uint8_t> img((size_t)res.width * res.height * 3);
                        for (uint32_t y = 0; y < res.height; ++y)
                            for (uint32_t x = 0; x < res.width; ++x)
                            {
                                uint32_t v;
                                std::memcpy(&v, p + (size_t)y * pitch + x * 4, 4);
                                const uint8_t* col = colours[std::min(v, 7u)];
                                std::memcpy(&img[((size_t)y * res.width + x) * 3], col, 3);
                            }
                        rb->Unmap(0, nullptr);
                        const std::string path = dumpPaths + "/paths_" + rn + ".ppm";
                        std::ofstream file(path, std::ios::binary);
                        file << "P6 " << res.width << " " << res.height << " 255" << (char)10;
                        file.write(reinterpret_cast<const char*>(img.data()), (std::streamsize)img.size());
                        logf("wrote %s\n", path.c_str());
                    }
                }
                shadow::setDebugPaths(gate.state, false);
                const shadow::VsmStats st = shadow::stats(gate.state);
                logf("%s warm-up: requested %u pages (%u by pixels), dirty %u (frame %llu), exhausted %u, free %u\n", rn.c_str(), st.requested, st.pixelRequested, st.dirty,
                     (unsigned long long)st.frame, st.exhausted, st.freePages);
                if (st.dirty != 0) fail("warm-up did not reach a clean page cache");
            }

            HarnessOptions options;
            options.frames = frames;
            options.label = "s_" + rn + (atmosphereEveryFrame ? "_atmo" : "");
            if (!out.empty()) options.outputDirectory = out;
            const HarnessResult result = harness.run(res, options, [&](RenderGraph& graph, const Resolution&, uint64_t f) {
                gate.frame.frameIndex = 100 + f;
                gate.frame.mainView = mainView(f);
                FrameResources resources;
                FrameServices services;  // no raster: a static camera requests no dirty page (checked below)
                FramePassContext fc = gate.context(graph, resources, services);
                ViewResources view;
                view.view = gate.frame.mainView;
                view.frameConstants = fc.frameConstantsFor(view.view);
                gate.importTargets(graph, view);
                tracks::atmosphere(fc);
                tracks::shadowPages(fc, view);
                tracks::shadowVisibility(fc, view);
                const TextureRef vis = view.shadowVisibility;  // stands in for M's shading kernel (keeps the pass live)
                graph.addPass("s.gate.consume", QueueType::Graphics, [&](PassBuilder& b) { b.use(vis, Use::SrvCompute); b.keep(); }, [](PassContext&) {});
                gate.frame.time += gate.frame.deltaTime;
            });
            harness.printSummary(result);
            const shadow::VsmStats st = shadow::stats(gate.state);
            logf("%s measured: requested %u pages, dirty %u\n", rn.c_str(), st.requested, st.dirty);
            const double px = st.pathNoCaster + st.pathRegionLit + st.pathRegionUmbra + st.pathSearchLit + st.pathFiltered + st.pathDiskLit + st.pathDiskUmbra;
            logf("  visibility paths (%% of %.2f M pixels): no caster %.1f, reach lit %.1f, reach umbra %.1f, search lit %.1f, disk lit %.1f,"
                 " disk umbra %.1f, filtered %.1f (pass 2 = search lit + disk + filtered)\n", px / 1e6, 100 * st.pathNoCaster / px, 100 * st.pathRegionLit / px,
                 100 * st.pathRegionUmbra / px, 100 * st.pathSearchLit / px, 100 * st.pathDiskLit / px, 100 * st.pathDiskUmbra / px, 100 * st.pathFiltered / px);
            if (st.dirty != 0) fail("pages became dirty during the measurement (the static-camera premise failed)");
            double sum = 0;
            for (const auto& [name, d] : result.passMs)
                if (name.rfind("s.", 0) == 0)
                {
                    logf("  %-28s median %.4f ms  P95 %.4f ms\n", name.c_str(), d.median, d.p95);
                    sum += d.median;
                }
            logf("  S passes (sum of medians)     %.4f ms\n", sum);
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
