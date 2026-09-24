#pragma once
// S track test frame: a FramePassContext without the other tracks (they are built in parallel and must not affect S's
// correctness tests), frame constants like FrameRenderer's, and GPU readback. Correctness only (no GPU lock).
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace unx::stest
{
using namespace unx::render;

#define S_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

class TestFrame
{
public:
    explicit TestFrame(bool debugLayer = true)
        : device([&] { DeviceOptions o; o.debugLayer = debugLayer; return o; }()),
          shaders(device, executableDirectory() / "shaders"),
          quality(QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality")),
          gpuScene(device)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kSlots * 1024;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "test frame constants");
        D3D12_RANGE none{ 0, 0 };
        check(constants->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map test constants");
    }

    void setScene(const scene::Scene& s)
    {
        sceneData = s;
        gpuScene.upload(sceneData);
    }

    // Same packing as FrameRenderer::allocateFrameConstants. Slots are recycled per frame; run() waits for the GPU.
    D3D12_GPU_VIRTUAL_ADDRESS frameConstantsFor(const ViewDesc& view)
    {
        if (slot >= kSlots) fail("test frame: more than %u views", kSlots);
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
        c.sunDirection = sceneData.sun.direction;
        c.sunIlluminance = sceneData.sun.illuminance;
        c.sunColor = sceneData.sun.color;
        c.sunAngularRadius = sceneData.sun.angularRadius;
        c.windDirection = sceneData.windDirection;
        c.windSpeed = sceneData.windSpeed;
        gpuScene.fill(c);
        std::memcpy(mapped + slot * 1024, &c, sizeof c);
        return constants->GetGPUVirtualAddress() + (slot++) * 1024;
    }

    // Records one frame: 'build' declares passes with the context; then executes and waits.
    void run(const std::function<void(FramePassContext&)>& build)
    {
        slot = 0;
        FrameResources resources;
        FrameServices services = testServices;
        FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, [this](const ViewDesc& v) { return frameConstantsFor(v); }, &trackState };
        build(fc);
        graph.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) device.queue((QueueType)q).waitCpu(graph.lastFence((QueueType)q));
        for (auto& f : afterFrame) f();
        afterFrame.clear();
        ++frame.frameIndex;
    }

    // Declares a copy of 'texture' (mip 0, all slices) into a readback buffer; the vector is filled after run().
    // Row pitch = align(width * bytesPerTexel, 256) (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
    static uint32_t rowPitch(uint32_t width, uint32_t bytesPerTexel) { return (width * bytesPerTexel + 255) & ~255u; }
    std::shared_ptr<std::vector<uint8_t>> readback(FramePassContext& fc, TextureRef texture)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        fc.graph.addPass("s.test.readback", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(texture, Use::CopySrc);
                             b.keep();
                         },
                         [this, texture, out](PassContext& c) {
                             ID3D12Resource* src = c.resource(texture);
                             D3D12_RESOURCE_DESC d = src->GetDesc();
                             D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                             UINT rows;
                             UINT64 rowBytes, total;
                             device.d3d()->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
                             ComPtr<ID3D12Resource> buffer = makeReadback(total);
                             D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                             dst.PlacedFootprint = fp;
                             D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                             s.SubresourceIndex = 0;
                             c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
                             afterFrame.push_back([buffer, out, total]() {
                                 void* p = nullptr;
                                 D3D12_RANGE r{ 0, (SIZE_T)total };
                                 check(buffer->Map(0, &r, &p), "map readback");
                                 out->assign((uint8_t*)p, (uint8_t*)p + total);
                                 D3D12_RANGE none{ 0, 0 };
                                 buffer->Unmap(0, &none);
                             });
                         });
        return out;
    }

    std::shared_ptr<std::vector<uint8_t>> readbackBuffer(FramePassContext& fc, BufferRef buffer, uint64_t bytes)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        fc.graph.addPass("s.test.readback.buffer", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(buffer, Use::CopySrc);
                             b.keep();
                         },
                         [this, buffer, bytes, out](PassContext& c) {
                             ComPtr<ID3D12Resource> rb = makeReadback(bytes);
                             c.cmd->CopyBufferRegion(rb.Get(), 0, c.resource(buffer), 0, bytes);
                             afterFrame.push_back([rb, out, bytes]() {
                                 void* p = nullptr;
                                 D3D12_RANGE r{ 0, (SIZE_T)bytes };
                                 check(rb->Map(0, &r, &p), "map readback");
                                 out->assign((uint8_t*)p, (uint8_t*)p + bytes);
                                 D3D12_RANGE none{ 0, 0 };
                                 rb->Unmap(0, &none);
                             });
                         });
        return out;
    }

    // Upload-heap-backed data copied into a new default buffer inside the frame (test inputs).
    BufferRef uploadBuffer(FramePassContext& fc, const void* data, uint64_t bytes, uint32_t stride, const char* name)
    {
        ComPtr<ID3D12Resource> staging = makeBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map staging");
        std::memcpy(p, data, bytes);
        staging->Unmap(0, nullptr);
        keepAlive.push_back(staging);
        BufferRef b = fc.graph.createBuffer(BufferDesc{ name, bytes, stride });
        fc.graph.addPass("s.test.upload", QueueType::Graphics,
                         [&](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                         [staging, b, bytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, staging.Get(), 0, bytes); });
        return b;
    }

    ComPtr<ID3D12Resource> makeBuffer(uint64_t bytes, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap{ type };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "test buffer");
        return r;
    }

    Device device;
    ShaderLibrary shaders;
    QualityConfig quality;
    GpuScene gpuScene;
    scene::Scene sceneData;
    RenderGraph graph{ device };
    FrameContext frame;
    FrameServices testServices;
    TrackState trackState;

private:
    ComPtr<ID3D12Resource> makeReadback(uint64_t bytes) { return makeBuffer(bytes, D3D12_HEAP_TYPE_READBACK); }
    static constexpr uint32_t kSlots = 16;
    ComPtr<ID3D12Resource> constants;
    uint8_t* mapped = nullptr;
    uint32_t slot = 0;
    std::vector<std::function<void()>> afterFrame;
    std::vector<ComPtr<ID3D12Resource>> keepAlive;
};

// RGBA32F texel of a readback (rowPitch from TestFrame::rowPitch, slices of 'height' rows).
inline float4 texel(const std::vector<uint8_t>& data, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t z = 0)
{
    const uint32_t pitch = TestFrame::rowPitch(width, 16);
    float4 v;
    std::memcpy(&v, data.data() + (size_t)z * pitch * height + (size_t)y * pitch + (size_t)x * 16, 16);
    return v;
}
} // namespace unx::stest
