#include "unx/shading/ShadingSystem.h"

#include "unx/core/Log.h"
#include "unx/material/MaterialSystem.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::shading
{
namespace
{
namespace model = scene::model;

float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

// The model's E table integrator (MaterialModel.cpp buildTable) with each sample split by its Schlick weight
// w = (1 - VoH)^5: A accumulates (1 - w), B accumulates w.
std::vector<float> buildSpecularTable()
{
    const uint32_t n = model::kAlbedoTableSize, samples = 4096;
    std::vector<float> table(2 * n * n);
    for (uint32_t ri = 0; ri < n; ++ri)
        for (uint32_t mi = 0; mi < n; ++mi)
        {
            const float r = (float)ri / (n - 1);
            const float mu = std::max((float)mi / (n - 1), 1e-4f);
            const float alpha = model::alphaFromRoughness(r);
            const float3 v{ std::sqrt(1 - mu * mu), 0, mu };
            const float3 vh = normalize(float3{ alpha * v.x, alpha * v.y, v.z });
            const float lensq = vh.x * vh.x + vh.y * vh.y;
            const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
            const float3 t2 = cross(vh, t1);
            const float g1v = 2 * mu / (mu + std::sqrt(alpha * alpha + (1 - alpha * alpha) * mu * mu));
            double a = 0, b = 0;
            for (uint32_t s = 0; s < samples; ++s)
            {
                const float u1 = (s + 0.5f) / samples, u2 = radicalInverse(s);
                const float radius = std::sqrt(u1), phi = 2 * model::kPi * u2;
                const float p1 = radius * std::cos(phi);
                const float sBlend = 0.5f * (1 + vh.z);
                const float p2 = (1 - sBlend) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + sBlend * radius * std::sin(phi);
                const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
                const float3 h = normalize(float3{ alpha * nh.x, alpha * nh.y, std::max(0.0f, nh.z) });
                const float VoH = dot(v, h);
                const float3 l = h * (2 * VoH) - v;
                const float NoL = l.z;
                if (NoL <= 0) continue;
                const double weight = 4.0 * model::visibilitySmithGgxCorrelated(mu, NoL, alpha) * NoL * mu / g1v;
                const double w = std::pow(1.0 - std::clamp((double)VoH, 0.0, 1.0), 5.0);
                a += weight * (1 - w);
                b += weight * w;
            }
            table[2 * (ri * n + mi)] = (float)(a / samples);
            table[2 * (ri * n + mi) + 1] = (float)(b / samples);
        }
    return table;
}

struct SpecularLut
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> buffer;
    uint32_t srv = gpu::kNone;
    ~SpecularLut()
    {
        if (!device) return;
        device->deferRelease(buffer);
        DescriptorHeaps* h = &device->descriptors();
        const uint32_t s = srv;
        device->deferCall([h, s] { h->freeResource(s); });
    }
    void ensure(Device& d)
    {
        if (buffer) return;
        device = &d;
        const std::vector<float>& t = specularAlbedoTable();
        const uint64_t bytes = t.size() * sizeof(float);
        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)), "M specular LUT");
        buffer->SetName(L"M specular albedo LUT");
        ComPtr<ID3D12Resource> staging;
        check(d.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)), "M LUT staging");
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map M LUT staging");
        std::memcpy(p, t.data(), bytes);
        staging->Unmap(0, nullptr);
        CommandList cl = d.acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, bytes);
        const uint64_t fence = d.submit(cl);
        d.queue(QueueType::Graphics).waitCpu(fence);
        srv = d.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.NumElements = (UINT)(t.size() / 2);
        sd.Buffer.StructureByteStride = 8;
        d.d3d()->CreateShaderResourceView(buffer.Get(), &sd, d.descriptors().resourceCpu(srv));
    }
};

void checkQuality(const QualityConfig& q)
{
    if (q.string("shading.tonemap") != "pbr_neutral") fail("shading.tonemap: only \"pbr_neutral\" (INTERFACES 8.4) is implemented");
    if (q.string("shading.output_encoding") != "srgb") fail("shading.output_encoding: only \"srgb\" (INTERFACES 7.5) is implemented");
    (void)q.integer("shading.analytic_lights_max");  // S reads it for the froxel lists (INTERFACES 9); local lights come with them
}

uint32_t experimentMask(const QualityConfig& q)
{
    const int64_t m = q.integer("shading.experiment_disable");
    static bool logged = false;
    if (m != 0 && !logged)
    {
        logf("M shading: experiment mask %lld leaves terms out (cost attribution run, not an image)\n", (long long)m);
        logged = true;
    }
    return (uint32_t)m;
}
} // namespace

const std::vector<float>& specularAlbedoTable()
{
    static const std::vector<float> t = buildSpecularTable();
    return t;
}

void shade(FramePassContext& fc, ViewResources& view)
{
    if (!view.color.valid()) fail("M.shading: the view has no colour target");
    checkQuality(fc.quality);
    const material::ResolveOutputs& o = material::resolveOutputs(fc, view);
    SpecularLut& lut = fc.state<SpecularLut>("M.specularLut");
    lut.ensure(fc.device);
    ID3D12CommandSignature* signature = material::dispatchSignature(fc);

    const bool linear = view.view.kind != gpu::ViewKind::Main || fc.frame.outputLinearHdr;
    ID3D12PipelineState* opaque = fc.shaders.compute(linear ? "Passes/Shading/ShadeOpaque.OUTPUT1" : "Passes/Shading/ShadeOpaque.OUTPUT0");
    ID3D12PipelineState* sky = fc.shaders.compute(linear ? "Passes/Shading/ShadeSky.OUTPUT1" : "Passes/Shading/ShadeSky.OUTPUT0");
    const FrameResources r = fc.resources;
    const bool atmosphere = r.transmittanceLut.valid() && r.multiScatterLut.valid() && r.skyViewLut.valid() && r.aerialPerspective.valid();
    const ViewResources v = view;
    const uint32_t tileCount = o.tilesX * o.tilesY, lutSrv = lut.srv, experiment = experimentMask(fc.quality);
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;

    // Planar reflection views are timed apart (their cost is R's reflection budget, ARCHITECTURE 2.6 C_planar).
    fc.graph.addPass(view.view.kind == gpu::ViewKind::Main ? "m.shade" : "m.shade.planar", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(v.gbuffer, Use::SrvCompute);
                         b.use(v.depth, Use::SrvCompute);
                         b.use(o.materialWord, Use::SrvCompute);
                         if (o.emissive.valid()) b.use(o.emissive, Use::SrvCompute);
                         b.use(o.tiles, Use::SrvCompute);
                         b.use(o.tileArgs, Use::IndirectArgs);
                         b.use(v.color, Use::UavComputeDisjoint);
                         if (v.shadowVisibility.valid()) b.use(v.shadowVisibility, Use::SrvCompute);
                         if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
                         if (v.reflection.valid()) b.use(v.reflection, Use::SrvCompute);
                         if (r.giCache.valid() && v.view.kind == gpu::ViewKind::PlanarReflection) b.use(r.giCache, Use::SrvCompute);
                         if (atmosphere)
                             for (TextureRef t : { r.transmittanceLut, r.multiScatterLut, r.skyViewLut, r.aerialPerspective }) b.use(t, Use::SrvCompute);
                     },
                     [=](PassContext& c) {
                         const uint32_t none = gpu::kNone;
                         const uint32_t atm[4] = { atmosphere ? c.srv(r.transmittanceLut) : none, atmosphere ? c.srv(r.multiScatterLut) : none,
                                                   atmosphere ? c.srv(r.skyViewLut) : none, atmosphere ? c.srv(r.aerialPerspective) : none };
                         c.bindFrameConstants(cb);
                         ID3D12Resource* args = c.resource(o.tileArgs);
                         // Sky tiles.
                         {
                             const uint32_t k[8] = { c.srv(o.materialWord), c.uav(v.color), c.srv(o.tiles), (uint32_t)material::ShadeClass::Sky * tileCount,
                                                     atm[0], atm[1], atm[2], atm[3] };
                             c.cmd->SetPipelineState(sky);
                             c.computeConstants(k, 8);
                             c.cmd->ExecuteIndirect(signature, 1, args, (uint32_t)material::ShadeClass::Sky * sizeof(D3D12_DISPATCH_ARGUMENTS), nullptr, 0);
                         }
                         // Surface classes (Subsurface and Water use the opaque model until theirs are defined).
                         c.cmd->SetPipelineState(opaque);
                         for (material::ShadeClass cls : { material::ShadeClass::Opaque, material::ShadeClass::Subsurface, material::ShadeClass::Water })
                         {
                             const uint32_t k[19] = { c.srv(v.gbuffer), c.srv(v.depth), c.srv(o.materialWord), c.uav(v.color),
                                                      c.srv(o.tiles), (uint32_t)cls * tileCount, (uint32_t)cls, o.emissive.valid() ? c.srv(o.emissive) : none,
                                                      v.shadowVisibility.valid() ? c.srv(v.shadowVisibility) : none, v.screenProbes.valid() ? c.srv(v.screenProbes) : none,
                                                      v.reflection.valid() ? c.srv(v.reflection) : none,
                                                      (r.giCache.valid() && v.view.kind == gpu::ViewKind::PlanarReflection) ? c.srv(r.giCache) : none,
                                                      atm[0], atm[1], atm[2], atm[3], lutSrv, o.textureTableSrv, experiment };
                             c.computeConstants(k, 19);
                             c.cmd->ExecuteIndirect(signature, 1, args, (uint32_t)cls * sizeof(D3D12_DISPATCH_ARGUMENTS), nullptr, 0);
                         }
                     });
}
} // namespace unx::render::shading
