#include "EmptyFrameScene.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace unx::test
{
namespace
{
enum Kind : uint32_t
{
    kFloat = 0,
    kUint = 1,
    kBuffer = 2,
    kUnorm = 3,
    kFloat3d = 4,
};

struct R
{
    bool texture;
    uint32_t id;
    uint32_t kind;
};
R tex(TextureRef t, uint32_t kind) { return { true, t.id, kind }; }
R buf(BufferRef b) { return { false, b.id, kBuffer }; }

ComPtr<ID3D12Resource> committedTexture(Device& device, uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_BARRIER_LAYOUT layout, const wchar_t* name,
                                        DXGI_FORMAT clearFormat = DXGI_FORMAT_UNKNOWN)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = clearFormat;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, layout, clearFormat == DXGI_FORMAT_UNKNOWN ? nullptr : &clear, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "CreateCommittedResource3 (texture)");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> committedBuffer(Device& device, uint64_t size, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource3 (buffer)");
    r->SetName(name);
    return r;
}

MeshPipelineDesc meshDesc(const char* ps, std::vector<DXGI_FORMAT> rts, DXGI_FORMAT depth, bool depthWrite, bool conservative = false)
{
    MeshPipelineDesc d;
    d.meshShader = "Passes/Test/Triangle.ms";
    d.pixelShader = ps ? ps : "";
    d.renderTargets = std::move(rts);
    d.depthFormat = depth;
    d.depthWrite = depthWrite;
    d.cull = D3D12_CULL_MODE_NONE;
    d.conservative = conservative;
    return d;
}
} // namespace

EmptyFrameScene::EmptyFrameScene(Device& device, ShaderLibrary& shaders, uint32_t width, uint32_t height)
    : m_device(device), m_shaders(shaders), m_width(width), m_height(height)
{
    m_output = committedTexture(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_BARRIER_LAYOUT_COMMON, L"output", DXGI_FORMAT_R10G10B10A2_UNORM);
    // ARCHITECTURE 4.2: VSM page cache 192 MB (sun + local). 8192 x 6144 x 4 B.
    m_vsmPool = committedTexture(device, 8192, 6144, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, L"vsm pool",
                                 DXGI_FORMAT_D32_FLOAT);
    m_giCache = committedBuffer(device, 32ull << 20, L"gi cache");         // 200k x 160 B
    m_particleState = committedBuffer(device, 32ull << 20, L"particles");  // 524k x 64 B
    m_tlas = committedBuffer(device, 64ull << 20, L"tlas (stand-in)");
    shaders.compute("Passes/Test/Touch");
    shaders.mesh("visDepth", meshDesc("Passes/Test/Triangle.ps.OUT1", { DXGI_FORMAT_R32_UINT }, DXGI_FORMAT_D32_FLOAT, true));
    shaders.mesh("depthOnly", meshDesc(nullptr, {}, DXGI_FORMAT_D32_FLOAT, true));
    shaders.mesh("coverage", meshDesc("Passes/Test/Triangle.ps.OUT2", {}, DXGI_FORMAT_D32_FLOAT, false, true));
    shaders.mesh("colorF16", meshDesc("Passes/Test/Triangle.ps.OUT0", { DXGI_FORMAT_R16G16B16A16_FLOAT }, DXGI_FORMAT_UNKNOWN, false));
    shaders.mesh("colorOut", meshDesc("Passes/Test/Triangle.ps.OUT0", { DXGI_FORMAT_R10G10B10A2_UNORM }, DXGI_FORMAT_UNKNOWN, false));
}

void EmptyFrameScene::build(RenderGraph& g, uint64_t frame, bool fullGraph)
{
    const uint32_t W = m_width, H = m_height;
    ID3D12PipelineState* touchPso = m_shaders.compute("Passes/Test/Touch");
    uint32_t seed = (uint32_t)frame * 131u;
    uint32_t declared = 0;

    auto touch = [&](const char* name, QueueType q, std::vector<R> in, std::vector<R> out, std::vector<R> alsoRead = {}, bool keep = false, bool disjoint = false) {
        ++declared;
        const uint32_t s = ++seed;
        g.addPass(name, q,
                  [=](PassBuilder& b) {
                      for (const R& r : in) r.texture ? b.use(TextureRef{ r.id }, Use::SrvCompute) : b.use(BufferRef{ r.id }, Use::SrvCompute);
                      for (const R& r : alsoRead) r.texture ? b.use(TextureRef{ r.id }, Use::SrvCompute) : b.use(BufferRef{ r.id }, Use::SrvCompute);
                      const Use w = disjoint ? Use::UavComputeDisjoint : Use::UavCompute;
                      for (const R& r : out) r.texture ? b.use(TextureRef{ r.id }, w) : b.use(BufferRef{ r.id }, w);
                      if (keep) b.keep();
                  },
                  [=](PassContext& c) {
                      uint32_t k[16] = {};
                      k[0] = (uint32_t)std::min<size_t>(in.size(), 3);
                      k[1] = (uint32_t)std::min<size_t>(out.size(), 2);
                      k[2] = s;
                      for (uint32_t i = 0; i < k[0]; ++i)
                      {
                          k[4 + i] = in[i].texture ? c.srv(TextureRef{ in[i].id }) : c.srv(BufferRef{ in[i].id });
                          k[8 + i] = in[i].kind;
                      }
                      for (uint32_t j = 0; j < k[1]; ++j)
                      {
                          k[12 + j] = out[j].texture ? c.uav(TextureRef{ out[j].id }) : c.uav(BufferRef{ out[j].id });
                          k[14 + j] = out[j].kind;
                      }
                      c.cmd->SetPipelineState(touchPso);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    };

    struct RasterSpec
    {
        const char* pso;
        TextureRef color;
        TextureRef depth;
        bool depthWrite = true;
        bool clear = false;
        BufferRef uav;
        std::vector<R> reads;
        uint32_t vw = 0, vh = 0;
        uint32_t clearRects = 0;  // VSM page clears
        BufferRef indirect;
    };
    auto raster = [&](const char* name, RasterSpec spec) {
        ++declared;
        const uint32_t s = ++seed;
        ID3D12PipelineState* pso = m_shaders.mesh(spec.pso, {});
        g.addPass(name, QueueType::Graphics,
                  [=](PassBuilder& b) {
                      if (spec.color.valid()) b.use(spec.color, Use::RenderTarget);
                      if (spec.depth.valid()) b.use(spec.depth, spec.depthWrite ? Use::DepthWrite : Use::DepthRead);
                      if (spec.uav.valid()) b.use(spec.uav, Use::UavGraphics);
                      if (spec.indirect.valid()) b.use(spec.indirect, Use::IndirectArgs);
                      for (const R& r : spec.reads) r.texture ? b.use(TextureRef{ r.id }, Use::SrvGraphics) : b.use(BufferRef{ r.id }, Use::SrvGraphics);
                  },
                  [=](PassContext& c) {
                      D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
                      if (spec.color.valid()) rtv = c.rtv(spec.color);
                      if (spec.depth.valid()) dsv = spec.depthWrite ? c.dsv(spec.depth) : c.dsvReadOnly(spec.depth);
                      if (spec.clear)
                      {
                          const float zero[4] = {};
                          if (spec.color.valid()) c.cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                          if (spec.depth.valid()) c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                      }
                      if (spec.clearRects)
                      {
                          D3D12_RECT rects[64];
                          for (uint32_t i = 0; i < spec.clearRects; ++i)
                          {
                              uint32_t page = (s * 7919u + i * 104729u) % (64u * 48u);
                              rects[i] = { (LONG)((page % 64) * 128), (LONG)((page / 64) * 128), (LONG)((page % 64) * 128 + 128), (LONG)((page / 64) * 128 + 128) };
                          }
                          c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, spec.clearRects, rects);
                      }
                      c.cmd->OMSetRenderTargets(spec.color.valid() ? 1 : 0, spec.color.valid() ? &rtv : nullptr, FALSE, spec.depth.valid() ? &dsv : nullptr);
                      const uint32_t vw = spec.vw ? spec.vw : c.desc(spec.color.valid() ? spec.color : spec.depth).width;
                      const uint32_t vh = spec.vh ? spec.vh : c.desc(spec.color.valid() ? spec.color : spec.depth).height;
                      D3D12_VIEWPORT vp{ 0, 0, (float)vw, (float)vh, 0, 1 };
                      D3D12_RECT sc{ 0, 0, (LONG)vw, (LONG)vh };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.cmd->SetPipelineState(pso);
                      uint32_t k[8] = { s, vh, 0, 0, spec.uav.valid() ? c.uav(spec.uav) : 0, 0, 0, 0 };
                      c.graphicsConstants(k, 8);
                      c.cmd->DispatchMesh(1, 1, 1);
                  });
    };

    const TextureDesc outDesc{ "output", W, H, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM };
    TextureRef output = g.importTexture(m_output.Get(), outDesc, D3D12_BARRIER_LAYOUT_COMMON);
    if (!fullGraph)
    {
        touch("final", QueueType::Graphics, {}, { tex(output, kUnorm) });
        return;
    }

    // Persistent state.
    TextureRef vsmPool = g.importTexture(m_vsmPool.Get(), { "vsm pool", 8192, 6144, 1, 1, DXGI_FORMAT_D32_FLOAT }, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
    BufferRef giCache = g.importBuffer(m_giCache.Get(), { "gi cache", 32ull << 20, 0 });
    BufferRef particles = g.importBuffer(m_particleState.Get(), { "particles", 32ull << 20, 0 });
    BufferRef tlas = g.importBuffer(m_tlas.Get(), { "tlas", 64ull << 20, 0 });

    // Frame transients (ARCHITECTURE 4.2 sizes).
    auto T2 = [&](const char* n, uint32_t w, uint32_t h, DXGI_FORMAT f) { return g.createTexture({ n, w, h, 1, 1, f }); };
    auto T3 = [&](const char* n, uint32_t w, uint32_t h, uint16_t d, DXGI_FORMAT f) { return g.createTexture({ n, w, h, d, 1, f, D3D12_RESOURCE_DIMENSION_TEXTURE3D }); };
    auto B = [&](const char* n, uint64_t size) { return g.createBuffer({ n, size, 0 }); };
    const uint64_t px = (uint64_t)W * H;
    TextureRef depth = T2("depth", W, H, DXGI_FORMAT_D32_FLOAT);
    TextureRef vis = T2("vis id", W, H, DXGI_FORMAT_R32_UINT);
    TextureRef hiz = T2("hiz", W / 2, H / 2, DXGI_FORMAT_R32_FLOAT);
    TextureRef gbuf = T2("gbuffer", W, H, DXGI_FORMAT_R32G32_UINT);
    TextureRef visibility = T2("vsm visibility", W, H, DXGI_FORMAT_R32_UINT);
    TextureRef coverageCount = T2("coverage count", W, H, DXGI_FORMAT_R32_UINT);
    TextureRef coverageResolve = T2("coverage resolve", W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef particlesQ = T2("particles 1/4", W / 2, H / 2, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef transmittance = T2("sky transmittance", 256, 64, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef multiscatter = T2("sky multiscatter", 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef skyview = T2("sky view", 192, 108, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef aerial = T3("aerial perspective", 32, 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TextureRef froxels = T3("froxels", (W + 23) / 24, (H + 23) / 24, 64, DXGI_FORMAT_R16G16B16A16_FLOAT);
    BufferRef water = B("water fft", 3ull * 512 * 512 * 16);
    BufferRef particleSort = B("particle sort", 524288ull * 8);
    BufferRef lightBvh = B("light bvh", 512 * 64);
    BufferRef lightLists = B("froxel light lists", (uint64_t)((W + 23) / 24) * ((H + 23) / 24) * 64 * 4);
    BufferRef instList = B("instances phase 1", 100000 * 32);
    BufferRef instList2 = B("instances phase 2", 100000 * 32);
    BufferRef clusterList = B("clusters phase 1", 1000000 * 16);
    BufferRef clusterList2 = B("clusters phase 2", 1000000 * 16);
    BufferRef bandA = B("band A clusters", 1000000 * 8);
    BufferRef bandB = B("band B/C clusters", 1000000 * 8);
    BufferRef args = B("indirect args", 4096);
    BufferRef shadowCull = B("shadow clusters", 1000000 * 8);
    BufferRef tileLists = B("material tiles", px / 64 * 8);
    BufferRef pageRequests = B("vsm page requests", 1 << 20);
    BufferRef pageTable = B("vsm page table", 1 << 20);
    BufferRef fragments = B("coverage fragments", 12ull * 1000000 * 16 * px / 8294400);
    BufferRef sortedFragments = B("coverage fragments sorted", 12ull * 1000000 * 16 * px / 8294400);
    BufferRef prefix = B("coverage prefix", px * 4);
    BufferRef radix = B("coverage radix histogram", 1 << 20);
    BufferRef particleEdges = B("particle edge pixels", px / 16 * 8);
    BufferRef oit = B("transparent oit", px / 20 * 4 * 16);
    BufferRef giRays = B("gi rays", 500000 * 32);
    BufferRef giHits = B("gi hits", 500000 * 32);
    BufferRef reflSamples = B("reflection samples", 600000 * 32);
    BufferRef reflHits = B("reflection hits", 600000 * 32);
    BufferRef reflResult = B("reflection result", px * 3 / 2);
    BufferRef probes = B("screen probes", px / 64 * 128);
    BufferRef sortKeys = B("visibility page keys", px * 4);
    BufferRef sortHist = B("visibility page histogram", 1 << 20);
    BufferRef sortedPixels = B("visibility sorted pixels", px * 4);
    BufferRef shadeTiles = B("shading tiles", px / 64 * 8);
    BufferRef exposure = B("exposure histogram", 4096);

    const QueueType A = QueueType::Compute, G = QueueType::Graphics;

    // C0-C2: simulation slice, sky LUTs, light lists, dynamic TLAS (async).
    touch("sim.particles.emit", A, {}, { buf(particles) });
    touch("sim.particles.update", A, {}, { buf(particles) });
    touch("sim.particles.bin", A, {}, { buf(particles) });
    touch("sim.cloth.predict", A, {}, { buf(particles) });
    touch("sim.cloth.solve", A, {}, { buf(particles) });
    touch("sim.water.fft", A, {}, { buf(water) });
    touch("particles.sort.keys", A, { buf(particles) }, { buf(particleSort) });
    touch("particles.sort.scatter", A, {}, { buf(particleSort) });
    touch("sky.transmittance", A, {}, { tex(transmittance, kFloat) });
    touch("sky.multiscatter", A, { tex(transmittance, kFloat) }, { tex(multiscatter, kFloat) });
    touch("sky.view", A, { tex(transmittance, kFloat), tex(multiscatter, kFloat) }, { tex(skyview, kFloat) });
    touch("sky.aerial", A, { tex(transmittance, kFloat), tex(multiscatter, kFloat) }, { tex(aerial, kFloat3d) });
    touch("lights.bvh", A, {}, { buf(lightBvh) });
    touch("lights.froxel.lists", A, { buf(lightBvh) }, { buf(lightLists) });
    touch("lights.froxel.compact", A, {}, { buf(lightLists) });
    touch("lights.shadow.requests", A, { buf(lightBvh) }, { buf(pageRequests) });
    touch("tlas.proxy.refit", A, { buf(particles) }, { buf(tlas) });
    touch("tlas.dynamic.build", A, {}, { buf(tlas) });
    touch("tlas.reflection.set", A, {}, { buf(tlas) });

    // G1-G2: two-phase culling, band classification, band A raster, HiZ.
    touch("cull.instances.p1", G, {}, { buf(instList) });
    touch("cull.clusters.p1", G, { buf(instList) }, { buf(clusterList) });
    touch("cull.classify.p1", G, { buf(clusterList) }, { buf(bandA), buf(bandB) });
    touch("cull.args.p1", G, { buf(bandA) }, { buf(args) });
    raster("raster.bandA.p1", { "visDepth", vis, depth, true, true, {}, { buf(bandA) }, 0, 0, 0, args });
    touch("hiz.mip0", G, { tex(depth, kFloat) }, { tex(hiz, kFloat) });
    for (const char* mip : { "hiz.mip1", "hiz.mip2", "hiz.mip3", "hiz.mip4", "hiz.mip5", "hiz.mip6", "hiz.mip7", "hiz.mip8", "hiz.mip9", "hiz.mip10" })
        touch(mip, G, {}, { tex(hiz, kFloat) });
    touch("cull.instances.p2", G, { tex(hiz, kFloat) }, { buf(instList2) });
    touch("cull.clusters.p2", G, { buf(instList2), tex(hiz, kFloat) }, { buf(clusterList2) });
    touch("cull.classify.p2", G, { buf(clusterList2) }, { buf(bandA), buf(bandB) });
    touch("cull.args.p2", G, { buf(bandA) }, { buf(args) });
    raster("raster.bandA.p2", { "visDepth", vis, depth, true, false, {}, { buf(bandA) }, 0, 0, 0, args });
    // Per-cascade ranges of one list: disjoint writers.
    for (const char* c : { "cull.shadow.c0", "cull.shadow.c1", "cull.shadow.c2", "cull.shadow.local" }) touch(c, G, { buf(instList) }, { buf(shadowCull) }, {}, false, true);

    // G3: material resolve -> 8 B G-buffer, per material class.
    touch("material.classify", G, { tex(vis, kUint) }, { buf(tileLists) });
    for (const char* m : { "material.opaque", "material.foliage", "material.hair", "material.water", "material.sss", "material.decal", "material.emissive", "material.clearcoat" })
        touch(m, G, { tex(vis, kUint), tex(depth, kFloat), buf(tileLists) }, { tex(gbuf, kUint) }, {}, false, true);  // per-class tiles

    // G4: VSM pages (sun clipmap + locals): mark, allocate, invalidate, clear, depth-only raster.
    for (const char* m : { "vsm.mark.sun", "vsm.mark.local", "vsm.mark.coarse", "vsm.mark.camera" }) touch(m, G, { tex(depth, kFloat) }, { buf(pageRequests) });
    touch("vsm.alloc", G, { buf(pageRequests) }, { buf(pageTable) });
    touch("vsm.alloc.compact", G, {}, { buf(pageTable) });
    touch("vsm.alloc.lru", G, {}, { buf(pageTable) });
    touch("vsm.invalidate.dynamic", G, { buf(shadowCull) }, { buf(pageTable) });
    touch("vsm.invalidate.wind", G, {}, { buf(pageTable) });
    // One page: the clear API path. Clearing dirty pages is VSM work budgeted in ARCHITECTURE 2.3, not graph overhead.
    raster("vsm.clear", { "depthOnly", {}, vsmPool, true, false, {}, { buf(pageTable) }, 8192, 6144, 1 });
    for (const char* c : { "vsm.raster.c0", "vsm.raster.c1", "vsm.raster.c2", "vsm.raster.c3", "vsm.raster.local" })
        raster(c, { "depthOnly", {}, vsmPool, true, false, {}, { buf(shadowCull), buf(pageTable) }, 8192, 6144 });

    // G5: coverage layer (bands B/C): conservative raster + exact area + append, counting sort, bricks, resolve.
    touch("coverage.args", G, { buf(bandB) }, { buf(args) });
    raster("coverage.raster", { "coverage", {}, depth, false, false, fragments, { buf(bandB) }, 0, 0, 0, args });
    touch("coverage.count", G, { buf(fragments) }, { tex(coverageCount, kUint) });
    touch("coverage.prefix.up", G, { tex(coverageCount, kUint) }, { buf(prefix) });
    touch("coverage.prefix.down", G, {}, { buf(prefix) });
    touch("coverage.radix.histogram", G, { buf(fragments) }, { buf(radix) });
    touch("coverage.radix.scan", G, {}, { buf(radix) });
    touch("coverage.radix.scatter0", G, { buf(fragments), buf(radix) }, { buf(sortedFragments) });
    touch("coverage.radix.scatter1", G, { buf(radix) }, { buf(sortedFragments) });
    touch("coverage.bricks", G, { buf(bandB), tex(depth, kFloat) }, { buf(sortedFragments) });
    touch("coverage.resolve", G, { buf(sortedFragments), buf(prefix) }, { tex(coverageResolve, kFloat) });

    // G7: particles and transparent layers.
    raster("particles.layer", { "colorF16", particlesQ, {}, true, true, {}, { buf(particles), buf(particleSort) } });
    touch("particles.edges", G, { tex(particlesQ, kFloat), tex(depth, kFloat) }, { buf(particleEdges) });
    touch("transparent.oit.gather", G, { tex(depth, kFloat) }, { buf(oit) });
    touch("transparent.oit.resolve", G, {}, { buf(oit) });

    // C3-C5: GI cache update, reflections, froxel integration, probes (async; waits on the G-buffer).
    touch("gi.select", A, { buf(giCache) }, { buf(giRays) });
    touch("gi.bin", A, {}, { buf(giRays) });
    touch("gi.trace", A, { buf(giRays), buf(tlas) }, { buf(giHits) });
    touch("gi.update", A, { buf(giHits) }, { buf(giCache) });
    touch("gi.compact", A, {}, { buf(giCache) });
    touch("refl.classify", A, { tex(gbuf, kUint), tex(depth, kFloat) }, { buf(reflSamples) });
    touch("refl.tiles", A, {}, { buf(reflSamples) });
    touch("refl.trace", A, { buf(reflSamples), buf(tlas) }, { buf(reflHits) });
    touch("refl.resolve", A, { buf(reflHits), buf(giCache) }, { buf(reflResult) });
    touch("froxel.lists", A, { buf(lightLists) }, { tex(froxels, kFloat3d) });
    touch("froxel.inject", A, { buf(lightLists), tex(vsmPool, kFloat), tex(aerial, kFloat3d) }, { tex(froxels, kFloat3d) });
    touch("froxel.integrate", A, {}, { tex(froxels, kFloat3d) });
    touch("probes.fill", A, { buf(giCache), tex(depth, kFloat) }, { buf(probes) });
    touch("probes.nearOcclusion", A, { tex(depth, kFloat) }, { buf(probes) });
    touch("probes.temporal", A, {}, { buf(probes) });

    // G8: VSM visibility pass with page sort (4 B/px).
    touch("visibility.keys", G, { tex(depth, kFloat), buf(pageTable) }, { buf(sortKeys) });
    touch("visibility.histogram", G, { buf(sortKeys) }, { buf(sortHist) });
    touch("visibility.scan", G, {}, { buf(sortHist) });
    touch("visibility.scatter", G, { buf(sortKeys), buf(sortHist) }, { buf(sortedPixels) });
    touch("visibility.taps", G, { buf(sortedPixels), tex(vsmPool, kFloat), buf(pageTable) }, { tex(visibility, kUint) });
    touch("visibility.penumbra", G, { buf(sortedPixels) }, { tex(visibility, kUint) });

    // G9: shading per material class -> final RGB10A2.
    touch("shade.classify", G, { tex(vis, kUint) }, { buf(shadeTiles) });
    for (const char* m : { "shade.opaque", "shade.foliage", "shade.hair", "shade.water", "shade.sss", "shade.clearcoat", "shade.cloth", "shade.eye" })
    {
        std::vector<R> extra = { buf(reflResult), tex(froxels, kFloat3d), tex(skyview, kFloat), buf(shadeTiles) };
        if (std::string_view(m) == "shade.water") extra.push_back(buf(water));
        touch(m, G, { tex(gbuf, kUint), tex(visibility, kUint), buf(probes) }, { tex(output, kUnorm) }, extra, false, true);  // per-class tiles
    }

    // G10: exposure, edge/coverage composite, particles, UI.
    touch("exposure.histogram", G, { tex(gbuf, kUint) }, { buf(exposure) });
    touch("exposure.reduce", G, {}, { buf(exposure) });
    touch("exposure.adapt", G, {}, { buf(exposure) });
    touch("composite.edges", G, { tex(coverageResolve, kFloat), tex(vis, kUint), buf(exposure) }, { tex(output, kUnorm) });
    touch("composite.particles", G, { tex(particlesQ, kFloat), buf(particleEdges), buf(oit) }, { tex(output, kUnorm) });
    raster("ui", { "colorOut", output, {}, true, false, {}, {} });
    BufferRef hud = B("debug hud", 64 * 1024);
    touch("debug.hud", G, { buf(exposure) }, { buf(hud) }, {}, true);  // read back by the HUD next frame

    if (declared != kPassCount) fail("EmptyFrameScene declared %u passes, expected %u", declared, kPassCount);
}
} // namespace unx::test
