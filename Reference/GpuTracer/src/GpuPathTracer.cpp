// GPU reference path tracer: scene upload, acceleration structures, dispatch scheduling. The estimator itself is in
// ../shaders (camera paths, sun-caustic light paths, splat resolve) and ../shared (code shared with the C++ tests).
#include "unx/reference/GpuPathTracer.h"

#include "GpuSlice.h"

#include "Atmosphere.h"
#include "Lights.h"
#include "RtScene.h"

#include "shared/Types.hlsli"

#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

namespace unx::reference
{
namespace sh = unx::reference::shared;
using render::ComPtr;
using render::check;

static_assert(sizeof(sh::RtInstance) == 64);
static_assert(sizeof(sh::RtMesh) == 16);
static_assert(sizeof(sh::RtSubmesh) == 16);
static_assert(sizeof(sh::RtMaterial) == 80);
static_assert(sizeof(sh::RtTexture) == 16);
static_assert(sizeof(sh::RtLight) == 96);
static_assert(sizeof(sh::RtEmitTriangle) == 48);
static_assert(sizeof(sh::RtAtmosphere) == 96);

namespace
{
sh::float3 toSh(float3 v) { return { v.x, v.y, v.z }; }

struct Buffer
{
    ComPtr<ID3D12Resource> res;
    uint64_t size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    uint32_t view = sh::kRtNone;  // SRV or UAV descriptor index
};

// Root constants (Scene.hlsli RtRoot).
struct Root
{
    uint32_t constants, x0, y0, w, h, sampleBegin, sampleEnd, pathBase, pathCount, passIndex, halfBase, pad1;
};
static_assert(sizeof(Root) == 48);

enum class Kind
{
    Path,
    Caustic,
    Resolve,
};

struct WorkItem
{
    Kind kind;
    Root root;
    uint32_t groupsX, groupsY, groupsZ;
    double units;  // path samples, light paths or pixels (for the time model)
};
} // namespace

struct GpuPathTracer::Impl
{
    const scene::Scene& scene;
    std::filesystem::path repo;
    std::string what;
    GpuRenderInfo info;

    std::unique_ptr<render::Device> device;
    std::unique_ptr<render::ShaderLibrary> shaders;
    ID3D12PipelineState* psoPath = nullptr;
    ID3D12PipelineState* psoCaustic = nullptr;
    ID3D12PipelineState* psoResolve = nullptr;
    std::unique_ptr<gpu::GpuSlice> slice;

    // Scene.
    std::vector<Buffer> owned;  // every buffer (kept alive)
    sh::RtConstants constants{};
    Buffer constantsBuffer, accum[2], splat[2], counters, countersZero;
    Buffer blasArena, tlas, instanceDescs, scratch;
    Buffer countersReadback, timestampReadback;
    ComPtr<ID3D12QueryHeap> queryHeap;
    static constexpr uint32_t kMaxQueries = 512;
    ComPtr<ID3D12Resource> staging;
    uint8_t* stagingPtr = nullptr;
    static constexpr uint64_t kStagingBytes = 64ull << 20;
    bool caustics = false;
    uint32_t width = 0, height = 0;
    float sceneTime = -1;
    bool built = false;

    Impl(const scene::Scene& s, std::filesystem::path r, std::string w) : scene(s), repo(std::move(r)), what(std::move(w)) {}

    // --- device and buffers
    void createDevice()
    {
        if (device) return;
        render::DeviceOptions o;
        o.queuePriority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;  // a background job: never ahead of other sessions' work
        device = std::make_unique<render::Device>(o);
        info.adapter = device->caps().adapter;
        info.driver = device->caps().driver;
        D3D12_FEATURE_DATA_D3D12_OPTIONS o0{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
        check(device->d3d()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof o0), "CheckFeatureSupport(OPTIONS)");
        check(device->d3d()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1), "CheckFeatureSupport(OPTIONS1)");
        // Standard D3D12 capabilities the tracer needs beyond the renderer's floor (INTERFACES 2.6): double-precision
        // add/mul (accumulation, ray-sphere quadratics) and 64-bit integers (PCG32).
        if (!o0.DoublePrecisionFloatShaderOps) fail("gpu reference: the device has no double-precision shader operations");
        if (!o1.Int64ShaderOps) fail("gpu reference: the device has no 64-bit integer shader operations");
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        shaders = std::make_unique<render::ShaderLibrary>(*device, std::filesystem::path(exe).parent_path() / "shaders" / "Reference");
        psoPath = shaders->compute("PathTrace");
        psoCaustic = shaders->compute("Caustic");
        psoResolve = shaders->compute("Resolve");
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kMaxQueries;
        check(device->d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&queryHeap)), "CreateQueryHeap");
        timestampReadback = createBuffer(kMaxQueries * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, false);
        countersReadback = createBuffer(sh::kRtCounterCount * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, false);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        const D3D12_RESOURCE_DESC rd = bufferDesc(kStagingBytes, D3D12_RESOURCE_FLAG_NONE);
        check(device->d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)), "staging buffer");
        check(staging->Map(0, nullptr, (void**)&stagingPtr), "Map staging");
        // An empty repository root (photo mode inside a game) has no measurement-lock protocol: no slices.
        if (!repo.empty()) slice = std::make_unique<gpu::GpuSlice>(repo / ".gpulock", "E", what);  // E owns the tracer since the B11 handover
    }

    static D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = std::max<uint64_t>(bytes, 4);
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return d;
    }

    Buffer createBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, bool count = true)
    {
        Buffer b;
        b.size = std::max<uint64_t>(bytes, 4);
        D3D12_HEAP_PROPERTIES hp{ heap };
        const D3D12_RESOURCE_DESC rd = bufferDesc(b.size, flags);
        check(device->d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&b.res)), "CreateCommittedResource(buffer)");
        b.va = b.res->GetGPUVirtualAddress();
        if (count && heap == D3D12_HEAP_TYPE_DEFAULT) info.vramSceneBytes += b.size;
        return b;
    }

    void structuredView(Buffer& b, uint32_t stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)std::max<uint64_t>(1, b.size / stride);
        d.Buffer.StructureByteStride = stride;
        b.view = device->descriptors().allocateResource();
        device->d3d()->CreateShaderResourceView(b.res.Get(), &d, device->descriptors().resourceCpu(b.view));
    }
    void rawView(Buffer& b)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)(b.size / 4);
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        b.view = device->descriptors().allocateResource();
        device->d3d()->CreateShaderResourceView(b.res.Get(), &d, device->descriptors().resourceCpu(b.view));
    }
    void rawUav(Buffer& b)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = (UINT)(b.size / 4);
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        b.view = device->descriptors().allocateResource();
        device->d3d()->CreateUnorderedAccessView(b.res.Get(), nullptr, &d, device->descriptors().resourceCpu(b.view));
    }

    void submitWait(render::CommandList& cl)
    {
        const uint64_t v = device->submit(cl);
        device->queue(render::QueueType::Compute).waitCpu(v);
    }

    // Copies bytes (or zeros when data is null) into a default-heap buffer through the staging buffer.
    void upload(Buffer& dst, const void* data, uint64_t bytes, uint64_t dstOffset = 0)
    {
        for (uint64_t off = 0; off < bytes; off += kStagingBytes)
        {
            const uint64_t n = std::min(kStagingBytes, bytes - off);
            if (data) std::memcpy(stagingPtr, (const uint8_t*)data + off, n);
            else std::memset(stagingPtr, 0, n);
            render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
            cl.list->CopyBufferRegion(dst.res.Get(), dstOffset + off, staging.Get(), 0, n);
            submitWait(cl);
        }
    }
    template <class T>
    Buffer structured(const std::vector<T>& v)
    {
        Buffer b = createBuffer(std::max<uint64_t>(v.size(), 1) * sizeof(T), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        if (!v.empty()) upload(b, v.data(), v.size() * sizeof(T));
        structuredView(b, sizeof(T));
        owned.push_back(b);
        return b;
    }

    void readback(const Buffer& src, void* out, uint64_t bytes)
    {
        Buffer rb = createBuffer(std::min(bytes, kStagingBytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, false);
        for (uint64_t off = 0; off < bytes; off += kStagingBytes)
        {
            const uint64_t n = std::min(kStagingBytes, bytes - off);
            render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
            cl.list->CopyBufferRegion(rb.res.Get(), 0, src.res.Get(), off, n);
            submitWait(cl);
            void* p = nullptr;
            const D3D12_RANGE r{ 0, (SIZE_T)n };
            check(rb.res->Map(0, &r, &p), "Map readback");
            std::memcpy((uint8_t*)out + off, p, n);
            const D3D12_RANGE none{ 0, 0 };
            rb.res->Unmap(0, &none);
        }
    }

    void noteVram()
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO m{};
        if (SUCCEEDED(device->adapter()->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)))
            info.vramProcessPeakBytes = std::max<uint64_t>(info.vramProcessPeakBytes, m.CurrentUsage);
    }

    void ensureSlice()
    {
        if (!slice || slice->held()) return;
        info.lockWaitSeconds += slice->acquire();
    }

    // --- scene
    void build(float time);
    void buildAccelerationStructures(const std::vector<sh::RtMesh>& meshRecords, const std::vector<std::vector<uint8_t>>& opaqueBySubmesh,
                                     const std::vector<int32_t>& instanceRecord, const std::vector<uint32_t>& meshVertexCount);

    RenderOutput render(const ResolvedCamera& cam, const RenderSettings& st, const Progress& progress);

    // --- shutter time integral (README 6)
    ShutterMotion motion;
    bool motionActive = false;
    struct Retime
    {
        std::vector<sh::RtInstance> instances;  // the records build() uploaded (rigid rows are rewritten per time)
        Buffer instanceBuffer, positions, normals, tangents, emit, emitCdf, instanceDescs, scratch;
        std::vector<uint8_t> deformed;
        std::vector<int32_t> recordOf;  // deformed instance -> its mesh record
        std::vector<sh::RtMesh> meshRecords;
        std::vector<uint8_t> smoothMaterial;  // sun-caustic class per material
        std::vector<std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geoms;  // per record (deformed ones rebuilt per time)
        std::vector<uint64_t> blasOffset;
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> tlasDescs;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    } rt;
    // The scene at time t inside the shutter: instance rows, deformed copies and their BLAS, TLAS, caustic emitters,
    // camera; the constants are uploaded. Returns its wall time (ms).
    double retime(float t);
    void buildEmit(const std::vector<float3x4>& transforms, std::vector<sh::RtEmitTriangle>& emit, std::vector<double>& cdf, double& area) const;
    void dispatchSamples(uint32_t half, uint32_t begin, uint32_t k);  // half 2: both halves

    // --- progressive rendering (render() = start, pass until done, output; photo mode drives the same steps)
    struct Run;
    std::unique_ptr<Run> run;
    void start(const ResolvedCamera& cam, const RenderSettings& st, const Progress& progress);
    // One pass of at most maxHalfSamples samples per half (whole image, both halves), then progress / checkpoint.
    void pass(uint32_t maxHalfSamples);
    void runBatch();
    void add(const WorkItem& w);
    // The accumulation so far (mean over the samples done); final: releases the lock slice and the checkpoint.
    RenderOutput output(bool final);
};

void GpuPathTracer::Impl::build(float time)
{
    if (built && sceneTime == time) return;
    if (built) fail("gpu reference: one scene time per tracer (rebuild with a new GpuPathTracer)");
    const auto t0 = std::chrono::steady_clock::now();
    const scene::Scene& s = scene;
    scene::validate(s);
    for (const scene::Material& m : s.materials)
    {
        if (m.cls != scene::MaterialClass::Standard && m.cls != scene::MaterialClass::Foliage)
            fail("reference: material '%s' uses a class without a v1 model (INTERFACES 8.1 defines Standard and Foliage)", m.name.c_str());
        if (m.occlusionTexture != scene::kNone)
            fail("reference: material '%s' has an occlusion texture; its use is not defined in INTERFACES 8.1 v1", m.name.c_str());
    }
    auto materialAlpha = [&](uint32_t mat) { return s.materials[mat].alphaCutoff > 0 && s.materials[mat].baseColorTexture != scene::kNone; };

    // Materials.
    std::vector<sh::RtMaterial> mats(s.materials.size());
    for (size_t i = 0; i < s.materials.size(); ++i)
    {
        const scene::Material& m = s.materials[i];
        sh::RtMaterial& g = mats[i];
        g.baseColor = toSh(m.baseColor);
        g.cls = (uint32_t)m.cls;
        g.emissive = toSh(m.emissive);
        g.roughness = m.roughness;
        g.metallic = m.metallic;
        g.specular = m.specular;
        g.alphaCutoff = m.alphaCutoff;
        g.transmission = m.transmission;
        g.twoSided = m.twoSided ? 1 : 0;
        g.baseColorTexture = m.baseColorTexture;
        g.normalTexture = m.normalTexture;
        g.roughMetalTexture = m.roughMetalTexture;
        g.emissiveTexture = m.emissiveTexture;
        g.smooth = sunCausticMaterial(m) ? 1 : 0;
        g.alphaTested = materialAlpha((uint32_t)i) ? 1 : 0;
    }
    // Textures: decoded exactly as the CPU estimator decodes them (RtScene Texture).
    std::vector<sh::RtTexture> texs;
    std::vector<sh::float4> texels;
    for (const scene::Texture& t : s.textures)
    {
        const Texture dec(t);
        sh::RtTexture r{ dec.width(), dec.height(), dec.wrap() ? 1u : 0u, (uint32_t)texels.size() };
        for (const Texel& x : dec.texels()) texels.push_back({ x.r, x.g, x.b, x.a });
        texs.push_back(r);
    }
    // Deformed instances (skinned, or wind at this time): world-space copies, as on the CPU.
    uint64_t deformedTris = 0;
    uint32_t deformedCount = 0;
    std::vector<uint8_t> deformed(s.instances.size(), 0);
    for (size_t i = 0; i < s.instances.size(); ++i)
        if (instanceNeedsDeformation(s, s.instances[i]))
        {
            deformed[i] = 1;
            ++deformedCount;
            deformedTris += s.meshes[s.instances[i].mesh].indices.size() / 3;
        }
    if (deformedTris > RtScene::kMaxDeformedTriangles)
        fail("reference: %u deformed instances need %.1f M deformed triangles (limit %.0f M); render with --no-wind", deformedCount, deformedTris / 1e6,
             RtScene::kMaxDeformedTriangles / 1e6);

    // Meshes: shared records, then one per deformed instance.
    std::vector<sh::float3> positions, normals;
    std::vector<sh::float4> tangents;
    std::vector<sh::float2> uvs;
    std::vector<uint32_t> indices;
    std::vector<sh::RtMesh> meshRecords;
    std::vector<uint32_t> meshVertexCount;
    std::vector<sh::RtMeshSubmeshes> meshSubs(s.meshes.size());
    std::vector<sh::RtSubmesh> subs;
    for (size_t mi = 0; mi < s.meshes.size(); ++mi)
    {
        const scene::Mesh& m = s.meshes[mi];
        sh::RtMesh r{ (uint32_t)positions.size(), (uint32_t)indices.size(), m.tangents.empty() ? 0u : 1u, m.uv0.empty() ? 0u : 1u };
        for (size_t v = 0; v < m.positions.size(); ++v)
        {
            positions.push_back(toSh(m.positions[v]));
            normals.push_back(toSh(m.normals[v]));
            tangents.push_back(m.tangents.empty() ? sh::float4(0, 0, 0, 1) : sh::float4(m.tangents[v].x, m.tangents[v].y, m.tangents[v].z, m.tangents[v].w));
            uvs.push_back(m.uv0.empty() ? sh::float2(0, 0) : sh::float2(m.uv0[v].x, m.uv0[v].y));
        }
        indices.insert(indices.end(), m.indices.begin(), m.indices.end());
        meshRecords.push_back(r);
        meshVertexCount.push_back((uint32_t)m.positions.size());
        meshSubs[mi] = { (uint32_t)subs.size(), (uint32_t)m.submeshes.size() };
        uint32_t covered = 0;
        for (const scene::Submesh& sm : m.submeshes)
        {
            if (sm.indexOffset % 3 || sm.indexCount % 3) fail("gpu reference: mesh '%s' has a submesh not aligned to triangles", m.name.c_str());
            subs.push_back({ sm.indexOffset / 3, sm.material, 0, 0 });
            covered += sm.indexCount;
        }
        if (covered != m.indices.size()) fail("gpu reference: mesh '%s' has triangles outside its submeshes", m.name.c_str());
    }
    std::vector<int32_t> instanceRecord(s.instances.size(), -1);  // deformed instance -> its mesh record
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (!deformed[i]) continue;
        const scene::Mesh& m = s.meshes[s.instances[i].mesh];
        const DeformedGeometry dg = deformInstance(s, (uint32_t)i, time);
        sh::RtMesh r{ (uint32_t)positions.size(), meshRecords[s.instances[i].mesh].indexOffset, dg.tangents.empty() ? 0u : 1u, m.uv0.empty() ? 0u : 1u };
        for (size_t v = 0; v < dg.positions.size(); ++v)
        {
            positions.push_back(toSh(dg.positions[v]));
            normals.push_back(toSh(dg.normals[v]));
            tangents.push_back(dg.tangents.empty() ? sh::float4(0, 0, 0, 1) : sh::float4(dg.tangents[v].x, dg.tangents[v].y, dg.tangents[v].z, dg.tangents[v].w));
            uvs.push_back(m.uv0.empty() ? sh::float2(0, 0) : sh::float2(m.uv0[v].x, m.uv0[v].y));
        }
        instanceRecord[i] = (int32_t)meshRecords.size();
        meshRecords.push_back(r);
        meshVertexCount.push_back((uint32_t)dg.positions.size());
    }
    // Instances and material overrides; per (mesh, submesh): opaque unless an instance using it has an alpha material.
    std::vector<sh::RtInstance> insts(s.instances.size());
    std::vector<uint32_t> overrides;
    std::vector<std::vector<uint8_t>> opaque(meshRecords.size());
    for (size_t mi = 0; mi < meshRecords.size(); ++mi)
    {
        const size_t src = mi < s.meshes.size() ? mi : 0;
        (void)src;
    }
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) opaque[mi].assign(s.meshes[mi].submeshes.size(), 1);
    if (s.instances.size() >= (1u << 24)) fail("gpu reference: %zu instances exceed the 24-bit instance id", s.instances.size());
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        const scene::Mesh& m = s.meshes[in.mesh];
        sh::RtInstance& g = insts[i];
        g.row0 = { in.transform.m[0][0], in.transform.m[0][1], in.transform.m[0][2], in.transform.m[0][3] };
        g.row1 = { in.transform.m[1][0], in.transform.m[1][1], in.transform.m[1][2], in.transform.m[1][3] };
        g.row2 = { in.transform.m[2][0], in.transform.m[2][1], in.transform.m[2][2], in.transform.m[2][3] };
        g.mesh = deformed[i] ? (uint32_t)instanceRecord[i] : in.mesh;
        g.sourceMesh = in.mesh;
        g.overrideOffset = sh::kRtNone;
        if (!in.materialOverrides.empty())
        {
            g.overrideOffset = (uint32_t)overrides.size();
            overrides.insert(overrides.end(), in.materialOverrides.begin(), in.materialOverrides.end());
        }
        g.flags = deformed[i] ? (sh::kRtInstanceWorld | sh::kRtInstanceDeformed) : 0u;
        std::vector<uint8_t>& op = deformed[i] ? opaque[instanceRecord[i]] : opaque[in.mesh];
        if (op.empty()) op.assign(m.submeshes.size(), 1);
        for (size_t k = 0; k < m.submeshes.size(); ++k)
            if (materialAlpha(in.materialOverrides.empty() ? m.submeshes[k].material : in.materialOverrides[k])) op[k] = 0;
    }
    if (overrides.empty()) overrides.push_back(0);

    // Lights (LightSet: normalised lights, grid).
    const LightSet lights(s);
    std::vector<sh::RtLight> glights(std::max<size_t>(lights.count(), 1));
    for (uint32_t i = 0; i < (uint32_t)lights.count(); ++i)
    {
        const scene::Light& l = lights.light(i);
        sh::RtLight& g = glights[i];
        g.position = toSh(l.position);
        g.type = (uint32_t)l.type;
        g.forward = toSh(l.forward);
        g.intensity = l.intensity;
        g.right = toSh(l.right);
        g.range = l.range;
        g.up = toSh(lights.up(i));
        g.spotScale = lights.spotScale(i);
        g.color = toSh(l.color);
        g.spotOffset = lights.spotOffset(i);
        g.size = { l.size.x, l.size.y };
        g.castShadow = l.castShadow ? 1 : 0;
    }
    std::vector<uint32_t> cellStart = lights.cellStart(), cellLights = lights.cellLights();
    for (size_t c = 0; c + 1 < cellStart.size(); ++c)
        if (cellStart[c + 1] - cellStart[c] > lights.count()) fail("gpu reference: light cell %zu lists more lights than exist", c);
    if (cellStart.empty()) cellStart.push_back(0);
    if (cellLights.empty()) cellLights.push_back(0);

    // Sun-caustic emission set (PathTracer::Impl::buildCaustics): smooth triangles of rigid instances, world space.
    rt.deformed = deformed;
    rt.recordOf = instanceRecord;
    rt.meshRecords = meshRecords;
    rt.instances = insts;
    rt.smoothMaterial.assign(mats.size(), 0);
    for (size_t m = 0; m < mats.size(); ++m) rt.smoothMaterial[m] = mats[m].smooth ? 1 : 0;
    std::vector<sh::RtEmitTriangle> emit;
    std::vector<double> emitCdf;
    double emitArea = 0;
    {
        std::vector<float3x4> transforms(s.instances.size());
        for (size_t i = 0; i < s.instances.size(); ++i) transforms[i] = s.instances[i].transform;
        buildEmit(transforms, emit, emitCdf, emitArea);
    }
    // Atmosphere table (built in double by the CPU model) and the material albedo table.
    const AtmosphereModel atm(s.atmosphere);
    std::vector<sh::float3> atmTable(atm.table().size());
    for (size_t i = 0; i < atmTable.size(); ++i) atmTable[i] = { atm.table()[i][0], atm.table()[i][1], atm.table()[i][2] };
    const std::vector<float>& albedo = scene::model::directionalAlbedoTable();
    const double setupSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // --- GPU (inside a slice).
    ensureSlice();
    const auto g0 = std::chrono::steady_clock::now();
    sh::RtBindings& b = constants.b;
    rt.instanceBuffer = structured(insts);
    b.instances = rt.instanceBuffer.view;
    b.meshes = structured(meshRecords).view;
    b.meshSubmeshes = structured(meshSubs).view;
    b.submeshes = structured(subs).view;
    b.materialOverrides = structured(overrides).view;
    b.materials = structured(mats).view;
    if (texs.empty()) texs.push_back({ 1, 1, 0, 0 });
    if (texels.empty()) texels.push_back({ 1, 1, 1, 1 });
    b.textures = structured(texs).view;
    b.texels = structured(texels).view;
    const Buffer posBuf = structured(positions);
    b.positions = posBuf.view;
    rt.positions = posBuf;
    rt.normals = structured(normals);
    b.normals = rt.normals.view;
    rt.tangents = structured(tangents);
    b.tangents = rt.tangents.view;
    b.uvs = structured(uvs).view;
    const Buffer idxBuf = structured(indices);
    b.indices = idxBuf.view;
    b.albedoTable = structured(albedo).view;
    b.atmTable = structured(atmTable).view;
    b.lights = structured(glights).view;
    b.cellStart = structured(cellStart).view;
    b.cellLights = structured(cellLights).view;
    if (emit.empty()) emit.push_back({});
    rt.emit = structured(emit);
    b.emitTriangles = rt.emit.view;
    {
        std::vector<double> cdf = emitCdf;
        if (cdf.empty()) cdf.push_back(0);
        Buffer c = createBuffer(cdf.size() * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        upload(c, cdf.data(), cdf.size() * 8);
        rawView(c);
        owned.push_back(c);
        b.emitCdf = c.view;
        rt.emitCdf = c;
    }
    counters = createBuffer(sh::kRtCounterCount * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    upload(counters, nullptr, counters.size);
    rawUav(counters);
    b.counters = counters.view;
    countersZero = createBuffer(sh::kRtCounterCount * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    upload(countersZero, nullptr, countersZero.size);

    // Scene-wide constants (camera and image size are set per render).
    constants.emitCount = (uint32_t)emitCdf.size();
    uint64_t areaBits;
    std::memcpy(&areaBits, &emitArea, 8);
    constants.emitAreaLo = (uint32_t)areaBits;
    constants.emitAreaHi = (uint32_t)(areaBits >> 32);
    constants.instanceCount = (uint32_t)s.instances.size();
    {
        sh::RtAtmosphere& a = constants.atm;
        const scene::Atmosphere& p = s.atmosphere;
        a.R = p.bottomRadius;
        a.Rt = p.topRadius;
        const double h2 = (double)p.topRadius * p.topRadius - (double)p.bottomRadius * p.bottomRadius;
        a.H2 = (float)h2;
        a.H = (float)std::sqrt(h2);
        a.rayleighScaleHeight = p.rayleighScaleHeight;
        a.mieScaleHeight = p.mieScaleHeight;
        a.mieG = p.mieG;
        a.ozoneCenter = p.ozoneCenter;
        a.ozoneWidth = p.ozoneWidth;
        a.rayleighScattering = toSh(p.rayleighScattering);
        a.mieScattering = toSh(p.mieScattering);
        a.mieAbsorption = toSh(p.mieAbsorption);
        a.ozoneAbsorption = toSh(p.ozoneAbsorption);
        a.groundAlbedo = toSh(p.groundAlbedo);
    }
    {
        // PathTracer::Impl constructor, same operations.
        sh::RtSun& g = constants.sun;
        const float3 sunDir = normalize(s.sun.direction);
        const double th0 = s.sun.angularRadius;
        const double halfSin = std::sin(0.5 * th0);
        g.dir = toSh(sunDir);
        g.halfSin = (float)halfSin;
        g.solidAngle = (float)(2.0 * 3.14159265358979323846 * 2.0 * halfSin * halfSin);
        g.sin2 = (float)(std::sin(th0) * std::sin(th0));
        const float k = (float)(s.sun.illuminance / (3.14159265358979323846 * std::sin(th0) * std::sin(th0)));
        g.radiance = { s.sun.color.x * k, s.sun.color.y * k, s.sun.color.z * k };
        const float3 ref = std::fabs(sunDir.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
        const float3 t1 = normalize(cross(ref, sunDir));
        g.t1 = toSh(t1);
        g.t2 = toSh(cross(sunDir, t1));
        caustics = !emitCdf.empty() && g.solidAngle > 0 && (g.radiance.x > 0 || g.radiance.y > 0 || g.radiance.z > 0);
    }
    {
        sh::RtLightGrid& g = constants.grid;
        g.minCorner = toSh(lights.gridMin());
        g.cell = toSh(lights.gridCell());
        g.count = (uint32_t)lights.count();
        g.dimX = lights.gridDim()[0];
        g.dimY = lights.gridDim()[1];
        g.dimZ = lights.gridDim()[2];
    }

    // Acceleration structures over the original geometry.
    {
        std::vector<std::vector<uint8_t>> opaqueByRecord = opaque;
        for (size_t r = 0; r < opaqueByRecord.size(); ++r)
            if (opaqueByRecord[r].empty() && r < s.meshes.size()) opaqueByRecord[r].assign(s.meshes[r].submeshes.size(), 1);
        std::vector<int32_t> rec = instanceRecord;
        (void)posBuf;
        (void)idxBuf;
        buildAccelerationStructures(meshRecords, opaqueByRecord, rec, meshVertexCount);
    }
    b.tlas = tlas.view;
    noteVram();
    info.buildSeconds = setupSec + std::chrono::duration<double>(std::chrono::steady_clock::now() - g0).count();
    logf("gpu reference: scene on the GPU (%zu meshes, %u deformed, %zu instances, %zu emit triangles, %.0f MB) in %.1f s\n", s.meshes.size(), deformedCount,
         s.instances.size(), emitCdf.size(), info.vramSceneBytes / 1048576.0, info.buildSeconds);
    sceneTime = time;
    built = true;
}

void GpuPathTracer::Impl::buildAccelerationStructures(const std::vector<sh::RtMesh>& meshRecords, const std::vector<std::vector<uint8_t>>& opaqueBySubmesh,
                                                      const std::vector<int32_t>& instanceRecord, const std::vector<uint32_t>& meshVertexCount)
{
    const scene::Scene& s = scene;
    // Positions / indices buffers are the ones uploaded as StructuredBuffers (views b.positions / b.indices).
    const Buffer* posBuf = nullptr;
    const Buffer* idxBuf = nullptr;
    for (const Buffer& o : owned)
    {
        if (o.view == constants.b.positions) posBuf = &o;
        if (o.view == constants.b.indices) idxBuf = &o;
    }
    if (!posBuf || !idxBuf) fail("gpu reference: vertex buffers missing");

    // Which records need a BLAS: shared meshes used by rigid instances, and every deformed record.
    std::vector<uint8_t> needed(meshRecords.size(), 0);
    for (size_t i = 0; i < s.instances.size(); ++i) needed[instanceRecord[i] >= 0 ? (size_t)instanceRecord[i] : s.instances[i].mesh] = 1;
    // Geometry descriptions per record: one per submesh.
    auto recordSource = [&](size_t r) -> uint32_t {
        if (r < s.meshes.size()) return (uint32_t)r;
        for (size_t i = 0; i < s.instances.size(); ++i)
            if (instanceRecord[i] == (int32_t)r) return s.instances[i].mesh;
        return 0;
    };
    std::vector<std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geoms(meshRecords.size());
    std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO> pre(meshRecords.size());
    std::vector<uint64_t> offset(meshRecords.size(), 0);
    uint64_t arena = 0, scratchMax = 0;
    for (size_t r = 0; r < meshRecords.size(); ++r)
    {
        if (!needed[r]) continue;
        const scene::Mesh& m = s.meshes[recordSource(r)];
        for (size_t k = 0; k < m.submeshes.size(); ++k)
        {
            const scene::Submesh& sm = m.submeshes[k];
            if (sm.indexCount == 0) continue;
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = (k < opaqueBySubmesh[r].size() && opaqueBySubmesh[r][k]) ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            g.Triangles.VertexBuffer.StartAddress = posBuf->va + (uint64_t)meshRecords[r].vertexOffset * 12;
            g.Triangles.VertexBuffer.StrideInBytes = 12;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount = meshVertexCount[r];
            g.Triangles.IndexBuffer = idxBuf->va + ((uint64_t)meshRecords[r].indexOffset + sm.indexOffset) * 4;
            g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount = sm.indexCount;
            geoms[r].push_back(g);
        }
        if (geoms[r].size() != m.submeshes.size())
            fail("gpu reference: mesh '%s' has an empty submesh (BLAS geometry index must equal the submesh index)", m.name.c_str());
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = (UINT)geoms[r].size();
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = geoms[r].data();
        device->d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre[r]);
        offset[r] = arena;
        arena += (pre[r].ResultDataMaxSizeInBytes + 255) & ~255ull;
        scratchMax = std::max(scratchMax, pre[r].ScratchDataSizeInBytes);
    }
    // TLAS inputs.
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs(s.instances.size());
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin{};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tin.NumDescs = (UINT)descs.size();
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpre{};
    device->d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tpre);
    scratchMax = std::max(scratchMax, tpre.ScratchDataSizeInBytes);

    blasArena = createBuffer(arena, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    scratch = createBuffer(scratchMax, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // BLAS builds: batches of about 2 M triangles per command list (short GPU work items).
    {
        render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
        uint64_t batchTris = 0;
        for (size_t r = 0; r < meshRecords.size(); ++r)
        {
            if (!needed[r]) continue;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
            d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            d.Inputs.NumDescs = (UINT)geoms[r].size();
            d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            d.Inputs.pGeometryDescs = geoms[r].data();
            d.DestAccelerationStructureData = blasArena.va + offset[r];
            d.ScratchAccelerationStructureData = scratch.va;
            cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            cl.list->ResourceBarrier(1, &uav);  // the scratch buffer is reused by the next build
            for (const auto& g : geoms[r]) batchTris += g.Triangles.IndexCount / 3;
            if (batchTris >= 2'000'000)
            {
                submitWait(cl);
                cl = device->acquireCommandList(render::QueueType::Compute);
                batchTris = 0;
            }
        }
        submitWait(cl);
    }
    // TLAS: scene instance index as InstanceID; mask bit 0 every instance, bit 1 shadow casters; no culling.
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC& d = descs[i];
        const bool world = instanceRecord[i] >= 0;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) d.Transform[row][col] = world ? (row == col ? 1.0f : 0.0f) : in.transform.m[row][col];
        d.InstanceID = (UINT)i;
        d.InstanceMask = 1u | ((in.flags & scene::InstanceCastShadow) ? 2u : 0u);
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        const size_t r = world ? (size_t)instanceRecord[i] : in.mesh;
        d.AccelerationStructure = blasArena.va + offset[r];
    }
    instanceDescs = createBuffer(descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    upload(instanceDescs, descs.data(), descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    tlas = createBuffer(tpre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    {
        render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs = tin;
        d.Inputs.InstanceDescs = instanceDescs.va;
        d.DestAccelerationStructureData = tlas.va;
        d.ScratchAccelerationStructureData = scratch.va;
        cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        submitWait(cl);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.RaytracingAccelerationStructure.Location = tlas.va;
    tlas.view = device->descriptors().allocateResource();
    device->d3d()->CreateShaderResourceView(nullptr, &v, device->descriptors().resourceCpu(tlas.view));
    rt.geoms = geoms;
    rt.blasOffset = offset;
    rt.tlasDescs = descs;
    rt.tlasInputs = tin;
    if (motionActive)
    {
        // kept for re-timing (TLAS and deformed BLAS rebuilt per epoch)
        rt.scratch = scratch;
        rt.instanceDescs = instanceDescs;
    }
    else info.vramSceneBytes -= scratch.size + instanceDescs.size;  // the scratch and instance descriptions are not needed any more
    scratch = {};
    instanceDescs = {};
}

namespace
{
struct CheckpointHeader
{
    char magic[8] = { 'U', 'N', 'X', 'R', 'E', 'F', 'G', '1' };
    uint32_t width = 0, height = 0, spp = 0, samplesDone = 0;
    uint64_t seed = 0;
    uint64_t paths = 0, rays = 0, truncated = 0, nans = 0;
    double seconds = 0;
    char identity[256] = {};
};
} // namespace


void GpuPathTracer::Impl::buildEmit(const std::vector<float3x4>& transforms, std::vector<sh::RtEmitTriangle>& emit, std::vector<double>& emitCdf, double& emitArea) const
{
    const scene::Scene& s = scene;
    emit.clear();
    emitCdf.clear();
    emitArea = 0;
    for (uint32_t i = 0; i < (uint32_t)s.instances.size(); ++i)
    {
        if (rt.deformed[i]) continue;
        const scene::Instance& in = s.instances[i];
        const float3x4& xf = transforms[i];
        const scene::Mesh& mesh = s.meshes[in.mesh];
        for (size_t si = 0; si < mesh.submeshes.size(); ++si)
        {
            const scene::Submesh& sub = mesh.submeshes[si];
            const uint32_t mat = in.materialOverrides.empty() ? sub.material : in.materialOverrides[si];
            if (mat >= rt.smoothMaterial.size() || !rt.smoothMaterial[mat]) continue;
            for (uint32_t k = sub.indexOffset; k + 2 < sub.indexOffset + sub.indexCount; k += 3)
            {
                const float3 p0 = xf.transformPoint(mesh.positions[mesh.indices[k]]);
                const float3 p1 = xf.transformPoint(mesh.positions[mesh.indices[k + 1]]);
                const float3 p2 = xf.transformPoint(mesh.positions[mesh.indices[k + 2]]);
                const float3 c = cross(p1 - p0, p2 - p0);
                const double area = 0.5 * std::sqrt((double)dot(c, c));
                if (!(area > 0)) continue;
                emit.push_back({ toSh(p0), i, toSh(p1 - p0), k / 3, toSh(p2 - p0), (uint32_t)si });
                emitArea += area;
                emitCdf.push_back(emitArea);
            }
        }
    }
}

namespace
{
// A rigid transform with per-axis scale (the tick interpolation rule): rotation, scale along the object axes, translation.
struct Decomposed
{
    double q[4];  // x, y, z, w
    double scale[3];
    double t[3];
};
Decomposed decompose(const float3x4& m)
{
    Decomposed d{};
    double c[3][3];
    for (int col = 0; col < 3; ++col)
    {
        const double x = m.m[0][col], y = m.m[1][col], z = m.m[2][col];
        d.scale[col] = std::sqrt(x * x + y * y + z * z);
        if (!(d.scale[col] > 0)) fail("gpu reference: shutter motion of a transform with a zero axis");
        c[0][col] = x / d.scale[col];
        c[1][col] = y / d.scale[col];
        c[2][col] = z / d.scale[col];
    }
    for (int a = 0; a < 3; ++a)
        for (int b2 = a + 1; b2 < 3; ++b2)
        {
            const double dp = c[0][a] * c[0][b2] + c[1][a] * c[1][b2] + c[2][a] * c[2][b2];
            if (std::abs(dp) > 1e-4) fail("gpu reference: shutter motion of a sheared transform (the tick rule interpolates rotation and axis scales)");
        }
    const double det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
    if (det < 0)
    {
        d.scale[0] = -d.scale[0];
        for (int r = 0; r < 3; ++r) c[r][0] = -c[r][0];
    }
    // rotation matrix -> quaternion (Shepperd)
    const double tr = c[0][0] + c[1][1] + c[2][2];
    if (tr > 0)
    {
        const double S = std::sqrt(tr + 1) * 2;
        d.q[3] = 0.25 * S;
        d.q[0] = (c[2][1] - c[1][2]) / S;
        d.q[1] = (c[0][2] - c[2][0]) / S;
        d.q[2] = (c[1][0] - c[0][1]) / S;
    }
    else if (c[0][0] > c[1][1] && c[0][0] > c[2][2])
    {
        const double S = std::sqrt(1 + c[0][0] - c[1][1] - c[2][2]) * 2;
        d.q[3] = (c[2][1] - c[1][2]) / S;
        d.q[0] = 0.25 * S;
        d.q[1] = (c[0][1] + c[1][0]) / S;
        d.q[2] = (c[0][2] + c[2][0]) / S;
    }
    else if (c[1][1] > c[2][2])
    {
        const double S = std::sqrt(1 + c[1][1] - c[0][0] - c[2][2]) * 2;
        d.q[3] = (c[0][2] - c[2][0]) / S;
        d.q[0] = (c[0][1] + c[1][0]) / S;
        d.q[1] = 0.25 * S;
        d.q[2] = (c[1][2] + c[2][1]) / S;
    }
    else
    {
        const double S = std::sqrt(1 + c[2][2] - c[0][0] - c[1][1]) * 2;
        d.q[3] = (c[1][0] - c[0][1]) / S;
        d.q[0] = (c[0][2] + c[2][0]) / S;
        d.q[1] = (c[1][2] + c[2][1]) / S;
        d.q[2] = 0.25 * S;
    }
    for (int r = 0; r < 3; ++r) d.t[r] = m.m[r][3];
    return d;
}
float3x4 interpolate(const float3x4& a, const float3x4& b, double u)
{
    if (u <= 0) return a;
    if (u >= 1) return b;
    const Decomposed A = decompose(a), B = decompose(b);
    double qb[4] = { B.q[0], B.q[1], B.q[2], B.q[3] };
    double cosTheta = A.q[0] * qb[0] + A.q[1] * qb[1] + A.q[2] * qb[2] + A.q[3] * qb[3];
    if (cosTheta < 0)
    {
        for (double& x : qb) x = -x;
        cosTheta = -cosTheta;
    }
    double wa, wb;
    if (cosTheta > 1 - 1e-12)
    {
        wa = 1 - u;
        wb = u;
    }
    else
    {
        const double theta = std::acos(std::min(cosTheta, 1.0)), sn = std::sin(theta);
        wa = std::sin((1 - u) * theta) / sn;
        wb = std::sin(u * theta) / sn;
    }
    double q[4];
    double len = 0;
    for (int k = 0; k < 4; ++k)
    {
        q[k] = wa * A.q[k] + wb * qb[k];
        len += q[k] * q[k];
    }
    len = std::sqrt(len);
    for (double& x : q) x /= len;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                             { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                             { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    float3x4 out;
    for (int col = 0; col < 3; ++col)
    {
        const double sc = A.scale[col] + (B.scale[col] - A.scale[col]) * u;
        for (int r = 0; r < 3; ++r) out.m[r][col] = (float)(R[r][col] * sc);
    }
    for (int r = 0; r < 3; ++r) out.m[r][3] = (float)(A.t[r] + (B.t[r] - A.t[r]) * u);
    return out;
}
// Camera pose as a rigid transform (columns right, up, -forward; the renderer's view basis) and back.
float3x4 cameraPose(float3 position, float3 forward, float3 up)
{
    const float3 f = normalize(forward), r = normalize(cross(f, up)), u = cross(r, f);
    float3x4 m;
    const float3 cols[4] = { r, u, float3{ -f.x, -f.y, -f.z }, position };
    for (int col = 0; col < 4; ++col)
    {
        m.m[0][col] = cols[col].x;
        m.m[1][col] = cols[col].y;
        m.m[2][col] = cols[col].z;
    }
    return m;
}
double radicalInverse2(uint32_t i)
{
    uint32_t b = i;
    b = (b << 16) | (b >> 16);
    b = ((b & 0x00ff00ffu) << 8) | ((b & 0xff00ff00u) >> 8);
    b = ((b & 0x0f0f0f0fu) << 4) | ((b & 0xf0f0f0f0u) >> 4);
    b = ((b & 0x33333333u) << 2) | ((b & 0xccccccccu) >> 2);
    b = ((b & 0x55555555u) << 1) | ((b & 0xaaaaaaaau) >> 1);
    return b * (1.0 / 4294967296.0);
}
uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}
} // namespace


struct GpuPathTracer::Impl::Run
{
    ResolvedCamera cam;
    RenderSettings st;
    Progress progress;
    uint32_t W = 0, H = 0, halfSpp = 0;
    uint64_t pixels = 0;
    bool useCaustics = false;
    CheckpointHeader hdr;
    uint32_t done = 0;  // samples per half accumulated (with shutter motion: the smaller half)
    uint32_t doneHalf[2] = { 0, 0 };  // shutter motion: each half's own count
    uint32_t epochHalf[2] = { 0, 0 };  // shutter motion: epochs of each half (time sample index)
    double passMs = 0;  // GPU wall time of the last pass (epoch length rule)
    // Slow start (structural dispatch bound; 2026-09-26 TDR on D0: the first dispatches were sized from the initial cost
    // guess and ran for seconds): at most unitCap paths per dispatch, starting small, doubled only while every dispatch
    // of a batch measured under half the target, halved when one measured over twice it; one dispatch per batch until a
    // dispatch has been measured near the target (the time model then sizes batches).
    uint64_t unitCap = 4096;
    bool calibrated = false;
    RenderStats stats;
    // Time model (ms per unit) per kernel, refined from GPU timestamps; start conservative.
    double msPer[3] = { 2e-5, 2e-5, 2e-6 };
    uint64_t dispatchCount = 0;
    double dispatchMsSum = 0;
    std::chrono::steady_clock::time_point lastCheckpoint, lastProgress;
    std::vector<WorkItem> batch;
};

double GpuPathTracer::Impl::retime(float t)
{
    const auto t0 = std::chrono::steady_clock::now();
    const scene::Scene& s = scene;
    Run& R = *run;
    const double u = motion.close > motion.open ? std::clamp(((double)t - motion.open) / ((double)motion.close - motion.open), 0.0, 1.0) : 0.0;
    std::vector<float3x4> transforms(s.instances.size());
    for (size_t i = 0; i < s.instances.size(); ++i)
        transforms[i] = motion.instances.empty() ? s.instances[i].transform : interpolate(s.instances[i].transform, motion.instances[i], u);
    std::vector<std::vector<float3x4>> poses(s.skeletons.size());
    for (size_t k = 0; k < s.skeletons.size(); ++k)
        if (k < motion.skeletons.size() && !motion.skeletons[k].empty())
        {
            const std::vector<float3x4>& open = s.skeletons[k].jointToModel;
            if (motion.skeletons[k].size() != open.size()) fail("gpu reference: shutter pose of skeleton %zu has %zu joints, the scene %zu", k, motion.skeletons[k].size(), open.size());
            poses[k].resize(open.size());
            for (size_t j = 0; j < open.size(); ++j) poses[k][j] = interpolate(open[j], motion.skeletons[k][j], u);
        }
    ensureSlice();
    // rigid rows; deformed world copies (positions, normals, tangents at their records) and their BLAS
    std::vector<sh::RtInstance> insts = rt.instances;
    std::vector<size_t> rebuild;
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (!rt.deformed[i])
        {
            const float3x4& m = transforms[i];
            insts[i].row0 = { m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3] };
            insts[i].row1 = { m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3] };
            insts[i].row2 = { m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3] };
            continue;
        }
        const scene::Instance& in = s.instances[i];
        const std::vector<float3x4>* pose = (in.flags & scene::InstanceSkinned) && in.skeleton < poses.size() && !poses[in.skeleton].empty() ? &poses[in.skeleton] : nullptr;
        const DeformedGeometry dg = deformInstanceAt(s, (uint32_t)i, t, transforms[i], pose);
        const size_t r = (size_t)rt.recordOf[i];
        const uint64_t v0 = rt.meshRecords[r].vertexOffset;
        std::vector<sh::float3> pos(dg.positions.size()), nrm(dg.normals.size());
        std::vector<sh::float4> tan(dg.positions.size(), sh::float4(0, 0, 0, 1));
        for (size_t v = 0; v < dg.positions.size(); ++v)
        {
            pos[v] = toSh(dg.positions[v]);
            nrm[v] = toSh(dg.normals[v]);
            if (!dg.tangents.empty()) tan[v] = sh::float4(dg.tangents[v].x, dg.tangents[v].y, dg.tangents[v].z, dg.tangents[v].w);
        }
        upload(rt.positions, pos.data(), pos.size() * sizeof(sh::float3), v0 * sizeof(sh::float3));
        upload(rt.normals, nrm.data(), nrm.size() * sizeof(sh::float3), v0 * sizeof(sh::float3));
        upload(rt.tangents, tan.data(), tan.size() * sizeof(sh::float4), v0 * sizeof(sh::float4));
        rebuild.push_back(r);
    }
    upload(rt.instanceBuffer, insts.data(), insts.size() * sizeof(sh::RtInstance));
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (rt.deformed[i]) continue;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) rt.tlasDescs[i].Transform[row][col] = transforms[i].m[row][col];
    }
    upload(rt.instanceDescs, rt.tlasDescs.data(), rt.tlasDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    {
        render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        for (size_t r : rebuild)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
            d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            d.Inputs.NumDescs = (UINT)rt.geoms[r].size();
            d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            d.Inputs.pGeometryDescs = rt.geoms[r].data();
            d.DestAccelerationStructureData = blasArena.va + rt.blasOffset[r];
            d.ScratchAccelerationStructureData = rt.scratch.va;
            cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
            cl.list->ResourceBarrier(1, &uav);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs = rt.tlasInputs;
        d.Inputs.InstanceDescs = rt.instanceDescs.va;
        d.DestAccelerationStructureData = tlas.va;
        d.ScratchAccelerationStructureData = rt.scratch.va;
        cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        cl.list->ResourceBarrier(1, &uav);
        submitWait(cl);
    }
    // sun-caustic emitters of moved instances
    if (caustics && !motion.instances.empty())
    {
        std::vector<sh::RtEmitTriangle> emit;
        std::vector<double> cdf;
        double area = 0;
        buildEmit(transforms, emit, cdf, area);
        if (emit.size() != (size_t)constants.emitCount) fail("gpu reference: the emitting triangle set changed inside the shutter");
        upload(rt.emit, emit.data(), emit.size() * sizeof(sh::RtEmitTriangle));
        upload(rt.emitCdf, cdf.data(), cdf.size() * 8);
        uint64_t bits;
        std::memcpy(&bits, &area, 8);
        constants.emitAreaLo = (uint32_t)bits;
        constants.emitAreaHi = (uint32_t)(bits >> 32);
    }
    // camera at t
    {
        const ResolvedCamera& cam = R.cam;
        const float3x4 pose = interpolate(cameraPose(cam.position, cam.forward, cam.up), cameraPose(motion.cameraPosition, motion.cameraForward, motion.cameraUp), u);
        sh::RtCamera& c = constants.camera;
        c.right = { pose.m[0][0], pose.m[1][0], pose.m[2][0] };
        c.up = { pose.m[0][1], pose.m[1][1], pose.m[2][1] };
        c.forward = { -pose.m[0][2], -pose.m[1][2], -pose.m[2][2] };
        c.position = { pose.m[0][3], pose.m[1][3], pose.m[2][3] };
    }
    upload(constantsBuffer, &constants, sizeof constants);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}


void GpuPathTracer::Impl::start(const ResolvedCamera& cam, const RenderSettings& st, const Progress& progress)
{
    if (st.width == 0 || st.height == 0) fail("reference: empty resolution");
    if (st.samplesPerPixel < 2 || (st.samplesPerPixel & 1)) fail("reference: samples per pixel must be even (two halves), got %u", st.samplesPerPixel);
    if (st.russianRouletteStart == 0) fail("reference: russian roulette start bounce must be >= 1");
    if (cam.lensAperture < 0 || !std::isfinite(cam.lensAperture)) fail("reference: lens aperture %g m", cam.lensAperture);
    if (cam.lensAperture > 0 && !(cam.lensFocus > cam.nearPlane && std::isfinite(cam.lensFocus)))
        fail("reference: thin lens focus distance %g m must exceed the near plane %g m", cam.lensFocus, cam.nearPlane);
    if (motionActive && !st.checkpoint.empty()) fail("reference: checkpoints are not supported with shutter motion");
    if (motionActive && cam.time != motion.open) fail("reference: the camera's scene time %g must be the shutter's open time %g", cam.time, motion.open);
    createDevice();
    build(cam.time);
    run = std::make_unique<Run>();
    Run& R = *run;
    R.cam = cam;
    R.st = st;
    R.progress = progress;
    const uint32_t W = st.width, H = st.height;
    R.W = W;
    R.H = H;
    R.halfSpp = st.samplesPerPixel / 2;
    const uint64_t pixels = (uint64_t)W * H;
    R.pixels = pixels;

    // Per-render constants and accumulators.
    {
        sh::RtCamera& c = constants.camera;
        const float3 f = normalize(cam.forward);
        const float3 right = normalize(cross(f, cam.up));
        const float3 up = cross(right, f);
        c.position = toSh(cam.position);
        c.forward = toSh(f);
        c.right = toSh(right);
        c.up = toSh(up);
        c.tanHalfFov = std::tan(0.5f * cam.verticalFov);
        c.aspect = (float)W / (float)H;
        c.nearPlane = cam.nearPlane;
        c.exposure = 1.0f / (1.2f * std::pow(2.0f, cam.ev100));
        c.lensRadius = 0.5f * cam.lensAperture;
        c.focusDistance = cam.lensAperture > 0 ? cam.lensFocus : 0.0f;
    }
    constants.width = W;
    constants.height = H;
    constants.seedLo = (uint32_t)st.seed;
    constants.seedHi = (uint32_t)(st.seed >> 32);
    constants.rrStart = st.russianRouletteStart;
    constants.forced = st.forcedInScattering ? 1 : 0;
    R.useCaustics = st.sunCaustics && caustics;
    constants.caustics = R.useCaustics ? 1 : 0;
    constants.orderMin = st.volumeOrderMin;
    constants.orderMax = st.volumeOrderMax;
    constants.surfMin = st.surfaceOrderMin;
    constants.surfMax = st.surfaceOrderMax;

    ensureSlice();
    for (int h = 0; h < 2; ++h)
    {
        if (accum[h].size != pixels * 24)
        {
            accum[h] = createBuffer(pixels * 24, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            rawUav(accum[h]);
            splat[h] = createBuffer(pixels * 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            rawUav(splat[h]);
        }
        upload(splat[h], nullptr, pixels * 12);
    }
    constants.b.accum0 = accum[0].view;
    constants.b.accum1 = accum[1].view;
    constants.b.splat0 = splat[0].view;
    constants.b.splat1 = splat[1].view;

    // Checkpoint (resumable accumulation).
    CheckpointHeader& hdr = R.hdr;
    hdr.width = W;
    hdr.height = H;
    hdr.spp = st.samplesPerPixel;
    hdr.seed = st.seed;
    std::snprintf(hdr.identity, sizeof hdr.identity, "%s|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g|%.9g|%.9g|%.9g|rr%u|gpu", scene.name.c_str(), cam.position.x,
                  cam.position.y, cam.position.z, cam.forward.x, cam.forward.y, cam.forward.z, cam.up.x, cam.up.y, cam.up.z, cam.verticalFov, cam.nearPlane, cam.ev100,
                  cam.time, st.russianRouletteStart);
    if (cam.lensAperture > 0)  // pinhole identities stay as they were
    {
        const size_t n = std::strlen(hdr.identity);
        std::snprintf(hdr.identity + n, sizeof hdr.identity - n, "|lens%.9g,%.9g", cam.lensAperture, cam.lensFocus);
    }
    uint32_t& done = R.done;
    RenderStats& stats = R.stats;
    {
        std::vector<double> init(pixels * 3, 0.0);
        bool resumed = false;
        if (!st.checkpoint.empty() && std::filesystem::exists(st.checkpoint))
        {
            std::ifstream f(st.checkpoint, std::ios::binary);
            CheckpointHeader have;
            f.read(reinterpret_cast<char*>(&have), sizeof have);
            if (f && std::memcmp(have.magic, hdr.magic, 8) == 0 && have.width == W && have.height == H && have.spp == hdr.spp && have.seed == hdr.seed &&
                std::strcmp(have.identity, hdr.identity) == 0)
            {
                std::vector<double> h1(pixels * 3);
                f.read(reinterpret_cast<char*>(init.data()), (std::streamsize)(init.size() * 8));
                f.read(reinterpret_cast<char*>(h1.data()), (std::streamsize)(h1.size() * 8));
                if (f)
                {
                    upload(accum[0], init.data(), pixels * 24);
                    upload(accum[1], h1.data(), pixels * 24);
                    done = have.samplesDone / 2;
                    R.doneHalf[0] = R.doneHalf[1] = done;
                    stats.paths = have.paths;
                    stats.rays = have.rays;
                    stats.truncatedPaths = have.truncated;
                    stats.nanSamples = have.nans;
                    stats.seconds = have.seconds;
                    resumed = true;
                    logf("reference: resumed %s at %u spp\n", st.checkpoint.string().c_str(), have.samplesDone);
                }
            }
        }
        if (!resumed)
        {
            upload(accum[0], nullptr, pixels * 24);
            upload(accum[1], nullptr, pixels * 24);
        }
    }
    constantsBuffer = createBuffer(sizeof(sh::RtConstants), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, false);
    upload(constantsBuffer, &constants, sizeof constants);
    structuredView(constantsBuffer, sizeof(sh::RtConstants));
    noteVram();
    R.lastCheckpoint = R.lastProgress = std::chrono::steady_clock::now();
}

void GpuPathTracer::Impl::runBatch()
{
    Run& R = *run;
    std::vector<WorkItem>& batch = R.batch;
    RenderStats& stats = R.stats;
    const double tsFreq = (double)device->queue(render::QueueType::Compute).timestampFrequency();
    double batchMaxMs = 0;
    if (batch.empty()) return;
    ensureSlice();
    const auto t0 = std::chrono::steady_clock::now();
    render::CommandList cl = device->acquireCommandList(render::QueueType::Compute);
    ID3D12DescriptorHeap* heaps[] = { device->descriptors().resourceHeap(), device->descriptors().samplerHeap() };
    cl.list->SetDescriptorHeaps(2, heaps);
    cl.list->SetComputeRootSignature(device->rootSignature());
    for (size_t k = 0; k < batch.size(); ++k)
    {
        const WorkItem& w = batch[k];
        cl.list->SetPipelineState(w.kind == Kind::Path ? psoPath : w.kind == Kind::Caustic ? psoCaustic : psoResolve);
        Root r = w.root;
        r.constants = constantsBuffer.view;
        cl.list->SetComputeRoot32BitConstants(0, sizeof(Root) / 4, &r, 0);
        cl.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * k));
        cl.list->Dispatch(w.groupsX, w.groupsY, w.groupsZ);
        cl.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * k + 1));
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl.list->ResourceBarrier(1, &uav);
    }
    cl.list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, (UINT)(2 * batch.size()), timestampReadback.res.Get(), 0);
    submitWait(cl);
    // Counters: read and reset (explicit states: the buffer decayed to COMMON after the list above).
    {
        render::CommandList c2 = device->acquireCommandList(render::QueueType::Compute);
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = counters.res.Get();
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        c2.list->ResourceBarrier(1, &tb);
        c2.list->CopyBufferRegion(countersReadback.res.Get(), 0, counters.res.Get(), 0, sh::kRtCounterCount * 4);
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        c2.list->ResourceBarrier(1, &tb);
        c2.list->CopyBufferRegion(counters.res.Get(), 0, countersZero.res.Get(), 0, sh::kRtCounterCount * 4);
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        c2.list->ResourceBarrier(1, &tb);
        submitWait(c2);
        uint32_t* cnt = nullptr;
        const D3D12_RANGE rr{ 0, sh::kRtCounterCount * 4 };
        check(countersReadback.res->Map(0, &rr, (void**)&cnt), "Map counters");
        stats.rays += cnt[sh::kRtCounterRays];
        stats.nanSamples += cnt[sh::kRtCounterNans];
        stats.truncatedPaths += cnt[sh::kRtCounterTruncated];
        info.errors |= cnt[sh::kRtCounterErrors];
        const D3D12_RANGE none{ 0, 0 };
        countersReadback.res->Unmap(0, &none);
    }
    {
        uint64_t* ts = nullptr;
        const D3D12_RANGE rr{ 0, 16 * batch.size() };
        check(timestampReadback.res->Map(0, &rr, (void**)&ts), "Map timestamps");
        for (size_t k = 0; k < batch.size(); ++k)
        {
            const double ms = (double)(ts[2 * k + 1] - ts[2 * k]) * 1000.0 / tsFreq;
            const WorkItem& w = batch[k];
            info.longestDispatchMs = std::max(info.longestDispatchMs, ms);
            R.dispatchMsSum += ms;
            ++R.dispatchCount;
            if (w.units > 0 && ms > 0.05)
            {
                double& m = R.msPer[(int)w.kind];
                const double est = ms / w.units;
                m = m * 0.5 + est * 0.5;
                m = std::max(m, est * 0.8);  // react fast to slower regions
            }
            batchMaxMs = std::max(batchMaxMs, ms);
        }
        const D3D12_RANGE none{ 0, 0 };
        timestampReadback.res->Unmap(0, &none);
    }
    if (info.errors) fail("gpu reference: kernel error bits 0x%x (iteration limit reached; INTERFACES 3.6)", info.errors);
    {
        const double target = kTargetDispatchMs;
        if (batchMaxMs > 2 * target) R.unitCap = std::max<uint64_t>(R.unitCap / 2, 1024);
        else if (batchMaxMs < 0.5 * target) R.unitCap = std::min<uint64_t>(R.unitCap * 2, 1ull << 26);
        if (batchMaxMs >= 0.25 * target || R.unitCap >= (1ull << 26)) R.calibrated = true;
    }
    batch.clear();
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    stats.seconds += sec;
    info.gpuSeconds += sec;
    if (slice && slice->sliceExpired()) slice->release();
}

void GpuPathTracer::Impl::add(const WorkItem& w)
{
    Run& R = *run;
    R.batch.push_back(w);
    double est = 0;
    for (const WorkItem& b : R.batch) est += b.units * R.msPer[(int)b.kind];
    if (!R.calibrated || R.batch.size() * 2 >= kMaxQueries || est >= 250.0) runBatch();
}

void GpuPathTracer::Impl::dispatchSamples(uint32_t half, uint32_t begin, uint32_t k)
{
    Run& R = *run;
    const uint32_t W = R.W, H = R.H;
    const uint64_t pixels = R.pixels;
    const double target = kTargetDispatchMs;
    const bool useCaustics = R.useCaustics;
    double* msPer = R.msPer;
    const uint32_t halves = half == 2 ? 2u : 1u, halfBase = half == 2 ? 0u : half, end = begin + k;
    // Camera paths: rectangles of whole rows (or of columns of one row band), at most unitCap paths each.
    const uint64_t perPixel = (uint64_t)halves * k;
    const uint32_t cols = (uint32_t)std::clamp<uint64_t>(R.unitCap / perPixel, 8, W);
    for (uint32_t y0 = 0; y0 < H;)
    {
        const double rowMs = (double)cols * perPixel * msPer[(int)Kind::Path];
        const uint64_t capRows = std::max<uint64_t>(R.unitCap / ((uint64_t)cols * perPixel), 1);
        const uint32_t rows = (uint32_t)std::min<uint64_t>(std::clamp((uint32_t)(target / std::max(rowMs, 1e-9)), 1u, H - y0), cols < W ? 1 : capRows);
        for (uint32_t x0 = 0; x0 < W; x0 += cols)
        {
            const uint32_t wc = std::min(cols, W - x0);
            WorkItem w{};
            w.kind = Kind::Path;
            w.root = { 0, x0, y0, wc, rows, begin, end, 0, 0, 0, halfBase, 0 };
            w.groupsX = (wc + 7) / 8;
            w.groupsY = (rows + 7) / 8;
            w.groupsZ = halves;
            w.units = (double)wc * rows * perPixel;
            add(w);
        }
        y0 += rows;
    }
    // Sun-caustic light paths: W H per sample per half (the CPU's count), then the splat resolve.
    if (useCaustics)
    {
        for (uint32_t s = 0; s < k; ++s)
            for (uint64_t base = 0; base < pixels;)
            {
                const uint64_t n = std::min<uint64_t>(std::clamp<uint64_t>((uint64_t)(target / std::max(halves * msPer[(int)Kind::Caustic], 1e-9)), 64, pixels - base),
                                                      std::max<uint64_t>(R.unitCap / halves, 64));
                WorkItem w{};
                w.kind = Kind::Caustic;
                w.root = { 0, 0, 0, 0, 0, begin, end, (uint32_t)base, (uint32_t)n, begin + s, halfBase, 0 };
                w.groupsX = (uint32_t)((n + 63) / 64);
                w.groupsY = 1;
                w.groupsZ = halves;
                w.units = (double)halves * n;
                add(w);
                base += n;
            }
        for (uint64_t base = 0; base < pixels;)
        {
            const uint64_t n = std::min<uint64_t>(pixels - base, 1u << 22);
            WorkItem w{};
            w.kind = Kind::Resolve;
            w.root = { 0, 0, 0, 0, 0, 0, 0, (uint32_t)base, (uint32_t)n, 0, 0, 0 };
            w.groupsX = (uint32_t)((n + 63) / 64);
            w.groupsY = 1;
            w.groupsZ = 1;
            w.units = (double)n;
            add(w);
            base += n;
        }
    }
    runBatch();
    R.stats.paths += pixels * halves * k;
}

void GpuPathTracer::Impl::pass(uint32_t maxHalfSamples)
{
    Run& R = *run;
    const uint32_t halfSpp = R.halfSpp;
    const uint64_t pixels = R.pixels;
    const double target = kTargetDispatchMs;
    const RenderSettings& st = R.st;
    RenderStats& stats = R.stats;
    uint32_t& done = R.done;
    CheckpointHeader& hdr = R.hdr;
    if (done >= halfSpp || maxHalfSamples == 0) return;
    if (motionActive)
    {
        // One epoch: one half at its own time sample (van der Corput per half, shifted by the seed), then passes of that
        // half until the re-timing is at most ~10 % of the epoch.
        const uint32_t h = R.doneHalf[0] <= R.doneHalf[1] ? 0u : 1u;
        const uint64_t shiftBits = mix64(st.seed ^ (0x9E3779B97F4A7C15ull * (h + 1)));
        const double shift = (double)(shiftBits >> 11) * (1.0 / 9007199254740992.0);
        double u = radicalInverse2(R.epochHalf[h]++) + shift;
        u -= std::floor(u);
        const float t = motion.open + (float)((motion.close - motion.open) * u);
        const double retimeMs = retime(t);
        const double imageMs = (double)pixels * R.msPer[(int)Kind::Path];
        uint32_t passes = R.passMs > 0 ? (uint32_t)std::ceil(9 * retimeMs / R.passMs) : 1u;
        passes = std::clamp(passes, 1u, 64u);
        // at least 64 time samples per half whatever the re-timing costs (the time integral converges with the number of
        // epochs; a tiny image would otherwise spend a half's samples in a few long epochs)
        const uint32_t epochSamples = std::max(1u, halfSpp / 64u);
        uint32_t spent = 0;
        for (uint32_t k0 = 0; k0 < passes && R.doneHalf[h] < halfSpp && spent < epochSamples; ++k0)
        {
            const uint32_t k = std::clamp((uint32_t)(target / std::max(imageMs, 1e-6)), 1u,
                                          std::min({ halfSpp - R.doneHalf[h], 64u, maxHalfSamples, epochSamples - spent }));
            spent += k;
            const auto p0 = std::chrono::steady_clock::now();
            dispatchSamples(h, R.doneHalf[h], k);
            R.passMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
            R.doneHalf[h] += k;
        }
        done = std::min(R.doneHalf[0], R.doneHalf[1]);
        stats.samplesDone = R.doneHalf[0] + R.doneHalf[1];
        noteVram();
        const auto now = std::chrono::steady_clock::now();
        if (R.progress && (done >= halfSpp || std::chrono::duration<double>(now - R.lastProgress).count() >= 10))
        {
            stats.pausedSeconds = info.lockWaitSeconds;
            R.progress(stats);
            R.lastProgress = now;
        }
        return;
    }
    // Samples per half in this pass: a whole-image pass should take about one target dispatch or more.
    const double imageMs = (double)pixels * 2 * R.msPer[(int)Kind::Path];
    const uint32_t k = std::clamp((uint32_t)(target / std::max(imageMs, 1e-6)), 1u, std::min({ halfSpp - done, 64u, maxHalfSamples }));
    const uint32_t begin = done, end = done + k;
    dispatchSamples(2, begin, k);
    done = end;
    R.doneHalf[0] = R.doneHalf[1] = done;
    stats.samplesDone = 2 * done;
    noteVram();
    const auto now = std::chrono::steady_clock::now();
    if (R.progress && (done >= halfSpp || std::chrono::duration<double>(now - R.lastProgress).count() >= 10))
    {
        stats.pausedSeconds = info.lockWaitSeconds;
        R.progress(stats);
        R.lastProgress = now;
    }
    if (!st.checkpoint.empty() && done < halfSpp && std::chrono::duration<double>(now - R.lastCheckpoint).count() >= st.checkpointSeconds)
    {
        ensureSlice();
        std::vector<double> h0(pixels * 3), h1(pixels * 3);
        readback(accum[0], h0.data(), pixels * 24);
        readback(accum[1], h1.data(), pixels * 24);
        hdr.samplesDone = 2 * done;
        hdr.paths = stats.paths;
        hdr.rays = stats.rays;
        hdr.truncated = stats.truncatedPaths;
        hdr.nans = stats.nanSamples;
        hdr.seconds = stats.seconds;
        std::filesystem::create_directories(st.checkpoint.parent_path());
        const std::filesystem::path tmp = st.checkpoint.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(&hdr), sizeof hdr);
            f.write(reinterpret_cast<const char*>(h0.data()), (std::streamsize)(h0.size() * 8));
            f.write(reinterpret_cast<const char*>(h1.data()), (std::streamsize)(h1.size() * 8));
            if (!f) fail("reference: cannot write checkpoint %s", tmp.string().c_str());
        }
        std::filesystem::rename(tmp, st.checkpoint);
        R.lastCheckpoint = std::chrono::steady_clock::now();
    }
}

RenderOutput GpuPathTracer::Impl::output(bool final)
{
    Run& R = *run;
    const uint32_t W = R.W, H = R.H;
    const uint64_t pixels = R.pixels;
    RenderStats& stats = R.stats;
    const RenderSettings& st = R.st;
    ensureSlice();
    std::vector<double> sum[2] = { std::vector<double>(pixels * 3), std::vector<double>(pixels * 3) };
    readback(accum[0], sum[0].data(), pixels * 24);
    readback(accum[1], sum[1].data(), pixels * 24);
    if (final && slice)
    {
        slice->release();
        info.slices = slice->slices();
        info.longestSliceSeconds = slice->longestSliceSeconds();
    }
    info.dispatches = (uint32_t)R.dispatchCount;
    info.meanDispatchMs = R.dispatchCount ? R.dispatchMsSum / R.dispatchCount : 0;
    stats.pausedSeconds = info.lockWaitSeconds;

    RenderOutput out;
    out.stats = stats;
    const float exposure = 1.0f / (1.2f * std::pow(2.0f, R.cam.ev100));
    for (metrics::Image* img : { &out.image, &out.halfA, &out.halfB })
    {
        img->width = W;
        img->height = H;
        img->rgb.resize(pixels * 3);
    }
    const double inv0 = R.doneHalf[0] ? 1.0 / R.doneHalf[0] : 0.0, inv1 = R.doneHalf[1] ? 1.0 / R.doneHalf[1] : 0.0;
    for (size_t i = 0; i < pixels * 3; ++i)
    {
        const float a = (float)(sum[0][i] * inv0) * exposure, b = (float)(sum[1][i] * inv1) * exposure;
        out.halfA.rgb[i] = a;
        out.halfB.rgb[i] = b;
        out.image.rgb[i] = 0.5f * (a + b);
    }
    out.halvesRelMse = metrics::relMse(out.halfA, out.halfB);
    if (final && !st.checkpoint.empty()) std::filesystem::remove(st.checkpoint);
    return out;
}

RenderOutput GpuPathTracer::Impl::render(const ResolvedCamera& cam, const RenderSettings& st, const Progress& progress)
{
    start(cam, st, progress);
    while (run->done < run->halfSpp) pass(UINT32_MAX);
    return output(true);
}

GpuPathTracer::GpuPathTracer(const scene::Scene& scene, std::filesystem::path repoRoot, std::string what)
    : m_impl(std::make_unique<Impl>(scene, std::move(repoRoot), std::move(what)))
{
}
GpuPathTracer::~GpuPathTracer()
{
    if (m_impl && m_impl->device)
    {
        m_impl->device->waitIdle();
        if (m_impl->slice && m_impl->slice->held()) m_impl->slice->release();
    }
}
RenderOutput GpuPathTracer::render(const ResolvedCamera& camera, const RenderSettings& settings, const Progress& progress)
{
    return m_impl->render(camera, settings, progress);
}
const GpuRenderInfo& GpuPathTracer::info() const { return m_impl->info; }
void GpuPathTracer::start(const ResolvedCamera& camera, const RenderSettings& settings) { m_impl->start(camera, settings, {}); }
void GpuPathTracer::setMotion(const ShutterMotion& motion)
{
    if (m_impl->built) fail("gpu reference: setMotion() before the first start() or render()");
    if (motion.close < motion.open) fail("gpu reference: the shutter closes (%g) before it opens (%g)", motion.close, motion.open);
    if (!motion.instances.empty() && motion.instances.size() != m_impl->scene.instances.size())
        fail("gpu reference: shutter motion has %zu instance transforms for %zu instances", motion.instances.size(), m_impl->scene.instances.size());
    if (!motion.skeletons.empty() && motion.skeletons.size() != m_impl->scene.skeletons.size())
        fail("gpu reference: shutter motion has %zu skeleton poses for %zu skeletons", motion.skeletons.size(), m_impl->scene.skeletons.size());
    m_impl->motion = motion;
    m_impl->motionActive = motion.close > motion.open;
}
uint32_t GpuPathTracer::pass(uint32_t maxHalfSamples)
{
    if (!m_impl->run) fail("gpu reference: pass() before start()");
    m_impl->pass(maxHalfSamples);
    return 2 * m_impl->run->done;
}
uint32_t GpuPathTracer::samplesDone() const { return m_impl->run ? 2 * m_impl->run->done : 0; }
RenderOutput GpuPathTracer::current()
{
    if (!m_impl->run) fail("gpu reference: current() before start()");
    return m_impl->output(false);
}
} // namespace unx::reference
