// Track W water height clipmap correctness (FEATURES_GAME 1.8 gates (1), (2); OceanHeight.hlsli, OceanRefine.hlsli;
// the first run of new kernels under GpuLock -Kind correctness):
//   1. build (gate 2): the scatter covers every lattice point (no point needs the fallback, no triangle passes its loop
//      bound); at 10 000 random lattice points the polished rest position x0 solves x0 + D(x0) = w within 0.02 s_l and
//      H = water level + h(x0); the fold counter equals the points flagged unsolved; the flag histogram (polished,
//      mesh value at fold edges) and the residual distribution are printed
//   2. max/min mips equal a CPU pyramid of the read-back heights bit for bit (every level, every mip)
//   3. search (gate 1): 4096 rays per camera (grazing 0.5..10 degrees below the horizon, steep, and upward) against a
//      double-precision reference on the same read-back clipmap: the level rule and window containment as specified,
//      a cell walk that finds each cell's first sign change of y(t) - H(t) (three evaluations fix the quadratic; its
//      vertex catches a dip) and 60 bisection steps. No ray reaches the node-visit cap; hit or miss agree; depth
//      |t_gpu - t_ref| <= 1e-4 t_ref. Node visits are printed (median, 99th percentile, max)
//   --time: [performance] build + mips (12 levels) and a 3840 x 2160 pinhole search over open sea (median of 16 frames)
//   unx_test_water_oceanheighttests [--no-debug-layer] [--time | --warp]
#include "unx/water/LinearDispatch.h"
#include "unx/water/OceanHeight.h"

#include "unx/render/GpuProfiler.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::Ocean;
using unx::water::OceanDesc;
using unx::water::OceanHeight;
using unx::water::OceanHeightDesc;
using unx::water::OceanHeightView;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
constexpr uint32_t N = OceanHeight::kN, kMips = OceanHeight::kMips, kCells = N - 1;
constexpr double kPi = 3.14159265358979323846;

ComPtr<ID3D12Device> warpDevice()
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "WARP device");
    return device;
}
struct Gpu
{
    ComPtr<ID3D12Device> external;
    Device device;
    ShaderLibrary shaders;
    Gpu(bool debugLayer, bool warp)
        : external(warp ? warpDevice() : nullptr), device([&] {
              DeviceOptions o;
              o.debugLayer = debugLayer && !warp;
              o.externalDevice = external.Get();
              return o;
          }()),
          shaders(device, executableDirectory() / "shaders") {}
};
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
    return r;
}
struct Rng
{
    uint64_t s;
    double next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double(s >> 11) * (1.0 / 9007199254740992.0); }
};

// An upload buffer with a raw SRV over it (the probe's input; not a graph resource).
struct Input
{
    ComPtr<ID3D12Resource> resource;
    uint32_t srv = 0;
    Device* device = nullptr;
    Input(Device& d, const void* data, uint64_t bytes) : device(&d)
    {
        resource = buffer(d, bytes, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        check(resource->Map(0, nullptr, &mapped), "map probe input");
        std::memcpy(mapped, data, bytes);
        resource->Unmap(0, nullptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(std::max<uint64_t>(bytes, 256) / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        srv = d.descriptors().allocateResource();
        d.d3d()->CreateShaderResourceView(resource.Get(), &sd, d.descriptors().resourceCpu(srv));
    }
    ~Input()
    {
        device->deferRelease(resource);
        device->descriptors().freeResource(srv);
    }
};

struct Result
{
    std::vector<float> height;               // [level][z][x][4]
    std::vector<std::vector<float>> bounds;  // [level * kMips + mip][z][x][2]
    uint32_t folds = 0, overflow = 0;
    std::vector<float> samples, rays;        // probe outputs, 4 floats each
};

struct Scene
{
    OceanDesc ocean;
    OceanHeightDesc height;
    OceanHeightView view;
    double seconds = 37.25;
};

// One frame: ocean, clipmap, probe modes 0 and 1 (and 2 for timing), read back unless profiling.
Result frame(Gpu& gpu, Ocean& ocean, OceanHeight& clip, const Scene& scene, const std::vector<uint32_t>& samples, const std::vector<float>& rays, uint32_t mode2Pixels,
             const std::vector<float>& mode2Camera, GpuProfiler* profiler = nullptr, uint64_t frameIndex = 0)
{
    RenderGraph g(gpu.device);
    const auto fields = ocean.record(g, scene.seconds);
    const auto out = clip.record(g, fields, scene.ocean.lengths, scene.view);
    const uint32_t levels = scene.height.levels;
    ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/OceanHeightProbe");
    std::vector<std::unique_ptr<Input>> inputs;
    const float camera[2] = { scene.view.camera[0], scene.view.camera[2] };
    const OceanHeightDesc hd = scene.height;
    const float lengths[3] = { scene.ocean.lengths[0], scene.ocean.lengths[1], scene.ocean.lengths[2] };
    auto addProbe = [&](uint32_t mode, uint32_t count, const void* data, uint64_t bytes) -> BufferRef {
        inputs.push_back(std::make_unique<Input>(gpu.device, data, bytes));
        const uint32_t inputSrv = inputs.back()->srv;
        const BufferRef result = g.createBuffer({ "ocean height probe", uint64_t(count) * 16, 0 });
        const auto displacement = fields.displacement, slopes = fields.slopes;
        g.addPass(mode == 2 ? "ocean refine image" : "ocean height probe", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(out.height, Use::SrvCompute); pb.use(out.bounds, Use::SrvCompute); pb.use(out.params, Use::SrvCompute);
                      pb.use(result, Use::UavCompute);
                      if (mode == 2) pb.keep();
                  },
                  [=](PassContext& c) {
                      uint32_t k[16] = { c.srv(displacement), c.srv(out.height), c.uav(result), count };
                      std::memcpy(&k[4], camera, 8);
                      std::memcpy(&k[6], &hd.s0, 4);
                      std::memcpy(&k[7], &hd.waterLevel, 4);
                      std::memcpy(&k[8], lengths, 12);
                      k[12] = c.srv(out.params);
                      k[13] = inputSrv;
                      k[14] = mode;
                      k[15] = c.srv(slopes);
                      c.cmd->SetPipelineState(probe);
                      c.computeConstants(k, 16);
                      unx::water::dispatchLinear(c.cmd, (count + 63) / 64);
                  });
        return result;
    };
    Result r;
    const bool read = !profiler;
    BufferRef sampleOut, rayOut;
    if (!samples.empty()) sampleOut = addProbe(0, uint32_t(samples.size() / 4), samples.data(), samples.size() * 4);
    if (!rays.empty()) rayOut = addProbe(1, uint32_t(rays.size() / 8), rays.data(), rays.size() * 4);
    if (mode2Pixels) addProbe(2, mode2Pixels, mode2Camera.data(), mode2Camera.size() * 4);

    ComPtr<ID3D12Resource> rbHeight, rbBounds, rbSmall;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fpH(levels), fpB(size_t(levels) * kMips);
    const uint64_t sampleBytes = samples.size() * 4, rayBytes = rays.size() / 2 * 4;
    if (read)
    {
        auto footprints = [&](DXGI_FORMAT format, uint32_t mips, std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& fp) {
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = td.Height = N;
            td.DepthOrArraySize = UINT16(levels);
            td.MipLevels = UINT16(mips);
            td.Format = format;
            td.SampleDesc.Count = 1;
            UINT64 total = 0;
            gpu.device.d3d()->GetCopyableFootprints(&td, 0, UINT(fp.size()), 0, fp.data(), nullptr, nullptr, &total);
            return total;
        };
        rbHeight = buffer(gpu.device, footprints(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, fpH), D3D12_HEAP_TYPE_READBACK);
        rbBounds = buffer(gpu.device, footprints(DXGI_FORMAT_R32G32_FLOAT, kMips, fpB), D3D12_HEAP_TYPE_READBACK);
        rbSmall = buffer(gpu.device, 256 + sampleBytes + rayBytes, D3D12_HEAP_TYPE_READBACK);
        ID3D12Resource *h = rbHeight.Get(), *b = rbBounds.Get(), *sm = rbSmall.Get();
        const auto fpHc = fpH, fpBc = fpB;
        const TextureRef height = out.height, bounds = out.bounds;
        const BufferRef folds = out.folds;
        g.addPass("ocean height read", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(height, Use::CopySrc); pb.use(bounds, Use::CopySrc); pb.use(folds, Use::CopySrc);
                      if (sampleOut.valid()) pb.use(sampleOut, Use::CopySrc);
                      if (rayOut.valid()) pb.use(rayOut, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      for (size_t s = 0; s < fpHc.size(); ++s)
                      {
                          D3D12_TEXTURE_COPY_LOCATION dst{ h, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          dst.PlacedFootprint = fpHc[s];
                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(height), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          src.SubresourceIndex = UINT(s);
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                      }
                      for (size_t s = 0; s < fpBc.size(); ++s)
                      {
                          D3D12_TEXTURE_COPY_LOCATION dst{ b, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          dst.PlacedFootprint = fpBc[s];
                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(bounds), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          src.SubresourceIndex = UINT(s);
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                      }
                      c.cmd->CopyBufferRegion(sm, 0, c.resource(folds), 0, 8);
                      if (sampleOut.valid()) c.cmd->CopyBufferRegion(sm, 256, c.resource(sampleOut), 0, sampleBytes);
                      if (rayOut.valid()) c.cmd->CopyBufferRegion(sm, 256 + sampleBytes, c.resource(rayOut), 0, rayBytes);
                  });
    }
    if (profiler) profiler->beginFrame(frameIndex);
    g.execute(profiler);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    if (!read) return r;
    const uint8_t* p = nullptr;
    check(rbHeight->Map(0, nullptr, (void**)&p), "map heights");
    r.height.resize(size_t(levels) * N * N * 4);
    for (uint32_t l = 0; l < levels; ++l)
        for (uint32_t z = 0; z < N; ++z) std::memcpy(&r.height[(size_t(l) * N + z) * N * 4], p + fpH[l].Offset + uint64_t(z) * fpH[l].Footprint.RowPitch, N * 16);
    rbHeight->Unmap(0, nullptr);
    check(rbBounds->Map(0, nullptr, (void**)&p), "map bounds");
    r.bounds.resize(fpB.size());
    for (size_t s = 0; s < fpB.size(); ++s)
    {
        const uint32_t n = fpB[s].Footprint.Width;
        r.bounds[s].resize(size_t(n) * n * 2);
        for (uint32_t z = 0; z < n; ++z) std::memcpy(&r.bounds[s][size_t(z) * n * 2], p + fpB[s].Offset + uint64_t(z) * fpB[s].Footprint.RowPitch, n * 8);
    }
    rbBounds->Unmap(0, nullptr);
    check(rbSmall->Map(0, nullptr, (void**)&p), "map probe");
    std::memcpy(&r.folds, p, 4);
    std::memcpy(&r.overflow, p + 4, 4);
    r.samples.assign((const float*)(p + 256), (const float*)(p + 256 + sampleBytes));
    r.rays.assign((const float*)(p + 256 + sampleBytes), (const float*)(p + 256 + sampleBytes + rayBytes));
    rbSmall->Unmap(0, nullptr);
    return r;
}

// ---- double-precision search reference on the read-back clipmap ----
struct Reference
{
    const Scene& scene;
    const std::vector<float>& H;
    float spacing(uint32_t l) const { return scene.height.s0 * float(1u << l); }
    // Level origin exactly as the kernels compute it (float division by a power of two is exact).
    int32_t origin(uint32_t l, float camera) const { return int32_t(std::floor(camera / spacing(l))) - 256; }
    double at(uint32_t l, int32_t x, int32_t z) const { return H[((size_t(l) * N + uint32_t(z)) * N + uint32_t(x)) * 4]; }
    double bilinear(uint32_t l, int32_t cx, int32_t cz, double fx, double fz) const
    {
        const double h00 = at(l, cx, cz), h10 = at(l, cx + 1, cz), h01 = at(l, cx, cz + 1), h11 = at(l, cx + 1, cz + 1);
        return h00 + (h10 - h00) * fx + (h01 - h00) * fz + (h11 - h10 - h01 + h00) * fx * fz;
    }
    uint32_t rule(double t) const
    {
        const double half = 0.5 * std::max(t, 1e-3) * scene.view.pixelAngle;
        const int l = int(std::floor(std::log2(std::max(half / scene.height.s0, 1.0))));
        return uint32_t(std::clamp(l, 0, int(scene.height.levels) - 1));
    }
    // First t with y(t) < H(t) from t = 0, or -1 (the ray leaves the outermost level or passes t max).
    double first(const double o[3], const double v[3]) const
    {
        const uint32_t levels = scene.height.levels;
        const double tMax = scene.view.farDistance;
        double t = 0;
        uint32_t level = rule(0);
        for (;;)
        {
            if (t > tMax) return -1;
            level = std::max(level, rule(t));
            const double s = spacing(level);
            const double ox = origin(level, scene.view.camera[0]), oz = origin(level, scene.view.camera[2]);
            auto uOf = [&](double tt, int a) { return (o[a == 0 ? 0 : 2] + tt * v[a == 0 ? 0 : 2]) / s - (a == 0 ? ox : oz); };
            const double u0 = uOf(t, 0), u1 = uOf(t, 1);
            if (u0 < 0 || u1 < 0 || u0 >= kCells || u1 >= kCells)
            {
                if (level + 1 >= levels) return -1;
                ++level;
                continue;
            }
            int32_t cell[2] = { int32_t(std::floor(u0)), int32_t(std::floor(u1)) };
            const double ruleSwitch = level + 1 < levels ? std::ldexp(double(scene.height.s0), int(level) + 2) / scene.view.pixelAngle : std::numeric_limits<double>::infinity();
            const double d[2] = { v[0] / s, v[2] / s };
            bool leave = false;
            while (!leave)
            {
                double exit = std::numeric_limits<double>::infinity();
                int axis = -1;
                for (int a = 0; a < 2; ++a)
                {
                    if (d[a] == 0) continue;
                    const double boundary = d[a] > 0 ? cell[a] + 1 : cell[a];
                    const double te = t + (boundary - uOf(t, a)) / d[a];
                    if (te < exit) { exit = te; axis = a; }
                }
                const double end = std::min({ exit, ruleSwitch, tMax });
                auto f = [&](double tt) { return o[1] + tt * v[1] - bilinear(level, cell[0], cell[1], uOf(tt, 0) - cell[0], uOf(tt, 1) - cell[1]); };
                const double f0 = f(t);
                if (f0 < 0) return t;  // the surface is above the ray where this level takes over
                const double f1 = f(end), fm = f(0.5 * (t + end));
                double lo = t, hi = -1;
                if (f1 < 0) hi = end;
                else
                {
                    // f is quadratic on [t, end]: through (0, f0), (1/2, fm), (1, f1) in x = (tt - t) / (end - t).
                    const double A = 2 * f1 - 4 * fm + 2 * f0, B = 4 * fm - 3 * f0 - f1;
                    if (A > 0)
                    {
                        const double xv = -B / (2 * A);
                        if (xv > 0 && xv < 1 && f(t + xv * (end - t)) < 0) hi = t + xv * (end - t);
                    }
                }
                if (hi >= 0)
                {
                    for (int i = 0; i < 60; ++i) { const double mid = 0.5 * (lo + hi); (f(mid) < 0 ? hi : lo) = mid; }
                    return hi;
                }
                t = end;
                if (end >= tMax) return -1;
                if (end == ruleSwitch) { leave = true; continue; }
                cell[axis] += d[axis] > 0 ? 1 : -1;
                if (cell[axis] < 0 || cell[axis] >= int32_t(kCells)) leave = true;
            }
            // Next level (containment or the pixel rule): the loop re-evaluates both at t.
            if (level + 1 >= levels) return -1;
            ++level;
        }
    }
};

void checkBounds(const Result& r, uint32_t levels)
{
    for (uint32_t l = 0; l < levels; ++l)
    {
        std::vector<float> prev;
        for (uint32_t m = 0; m < kMips; ++m)
        {
            const uint32_t n = N >> m;
            std::vector<float> b(size_t(n) * n * 2);
            for (uint32_t z = 0; z < n; ++z)
                for (uint32_t x = 0; x < n; ++x)
                {
                    float mx, mn;
                    if (m == 0)
                    {
                        const uint32_t cx = std::min(x, kCells - 1), cz = std::min(z, kCells - 1);
                        auto h = [&](uint32_t i, uint32_t j) { return r.height[((size_t(l) * N + j) * N + i) * 4]; };
                        const float a = h(cx, cz), c = h(cx + 1, cz), d = h(cx, cz + 1), e = h(cx + 1, cz + 1);
                        mx = std::max(std::max(a, c), std::max(d, e));
                        mn = std::min(std::min(a, c), std::min(d, e));
                    }
                    else
                    {
                        const uint32_t pn = n * 2;
                        auto q = [&](uint32_t i, uint32_t j, int k) { return prev[(size_t(j) * pn + i) * 2 + k]; };
                        mx = std::max(std::max(q(2 * x, 2 * z, 0), q(2 * x + 1, 2 * z, 0)), std::max(q(2 * x, 2 * z + 1, 0), q(2 * x + 1, 2 * z + 1, 0)));
                        mn = std::min(std::min(q(2 * x, 2 * z, 1), q(2 * x + 1, 2 * z, 1)), std::min(q(2 * x, 2 * z + 1, 1), q(2 * x + 1, 2 * z + 1, 1)));
                    }
                    b[(size_t(z) * n + x) * 2] = mx;
                    b[(size_t(z) * n + x) * 2 + 1] = mn;
                }
            const auto& gpuB = r.bounds[size_t(l) * kMips + m];
            W_CHECK(std::memcmp(gpuB.data(), b.data(), b.size() * 4) == 0, "bounds level %u mip %u differ from the CPU pyramid", l, m);
            prev = b;
        }
    }
}

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size())))];
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, time = false, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--time") time = true;
            if (std::string(argv[i]) == "--warp") warp = true;
        }
        if (warp && time) fail("--time measures hardware; WARP runs correctness only");
        Gpu gpu(debugLayer, warp);
        if (warp) std::printf("device: WARP (correctness only)\n");
        Scene scene;
        scene.ocean.windSpeed = 10.0f;
        scene.ocean.windDirection = 0.4f;
        scene.ocean.seed = 3;
        scene.view.pixelAngle = float(kPi / 3 / 3840);  // 60 degrees across 3840 pixels
        scene.view.farDistance = 5000.0f;
        Ocean ocean(gpu.device, gpu.shaders, scene.ocean);
        OceanHeight clip(gpu.device, gpu.shaders, scene.height);

        if (time)
        {
            const uint32_t width = 3840, height = 2160;
            scene.view.camera[0] = 3.7f; scene.view.camera[1] = 12.0f; scene.view.camera[2] = -5.3f;
            const double pitch = -10 * kPi / 180;
            const float fwd[3] = { float(std::cos(pitch)), float(std::sin(pitch)), 0 }, up[3] = { float(-std::sin(pitch)), float(std::cos(pitch)), 0 };
            std::vector<float> cam = { scene.view.camera[0], scene.view.camera[1], scene.view.camera[2], float(std::tan(kPi / 6)), fwd[0], fwd[1], fwd[2], 0, 0, 0, 1, 0, up[0], up[1], up[2], 0 };
            std::memcpy(&cam[7], &width, 4);
            std::memcpy(&cam[11], &height, 4);
            GpuProfiler profiler(gpu.device, 1, 64);
            std::vector<double> build, image;
            for (uint64_t f = 0; f < 24; ++f)
            {
                scene.seconds = 37.25 + f / 165.0;
                frame(gpu, ocean, clip, scene, {}, {}, width * height, cam, &profiler, f);
                if (f >= 8 && profiler.lastCompleted())
                {
                    double b = 0, i = 0;
                    for (const auto& p : profiler.lastCompleted()->passes)
                    {
                        if (p.name == "ocean height scatter" || p.name == "ocean height resolve" || p.name == "ocean height bounds") b += p.durationMs();
                        if (p.name == "ocean refine image") i += p.durationMs();
                    }
                    build.push_back(b);
                    image.push_back(i);
                }
            }
            std::printf("[performance] clipmap build + mips (12 levels x 512^2): median %.4f ms (min %.4f, max %.4f); 3840x2160 pinhole search over open sea "
                        "(camera 12 m, pitch -10 deg, 60 deg fov, half sky): median %.4f ms (min %.4f, max %.4f) over %zu frames\n",
                        percentile(build, 0.5), percentile(build, 0), percentile(build, 1), percentile(image, 0.5), percentile(image, 0), percentile(image, 1), build.size());
            return 0;
        }

        struct Camera { const char* name; float x, y, z; };
        const Camera cameras[] = { { "12 m above the sea", 3.7f, 12.0f, -5.3f }, { "4 m, 1.7 km from the origin", 1500.3f, 4.0f, -800.7f } };
        for (const Camera& cam : cameras)
        {
            scene.view.camera[0] = cam.x; scene.view.camera[1] = cam.y; scene.view.camera[2] = cam.z;
            Rng rng{ 0x9E3779B97F4A7C15ull ^ uint64_t(cam.y * 1000) };
            constexpr uint32_t kSamples = 10000, kRays = 4096;
            std::vector<uint32_t> samples(kSamples * 4);
            for (uint32_t i = 0; i < kSamples; ++i)
            {
                samples[4 * i] = uint32_t(rng.next() * scene.height.levels) % scene.height.levels;
                samples[4 * i + 1] = uint32_t(rng.next() * N) % N;
                samples[4 * i + 2] = uint32_t(rng.next() * N) % N;
            }
            std::vector<float> rays(kRays * 8);
            for (uint32_t i = 0; i < kRays; ++i)
            {
                double elevation;
                const uint32_t kind = i % 10;
                if (kind < 7) elevation = -0.5 * std::pow(20.0, rng.next());      // grazing: 0.5 .. 10 degrees down
                else if (kind < 9) elevation = -(10 + 79 * rng.next());           // steep
                else elevation = 5 * rng.next();                                  // up: misses
                const double e = elevation * kPi / 180, az = 2 * kPi * rng.next();
                const float r[8] = { cam.x, cam.y, cam.z, 0, float(std::cos(e) * std::cos(az)), float(std::sin(e)), float(std::cos(e) * std::sin(az)), 0 };
                std::memcpy(&rays[8 * i], r, 32);
            }
            const Result r = frame(gpu, ocean, clip, scene, samples, rays, 0, {});

            // 1. build
            uint64_t flagCount[4] = {};
            for (size_t i = 0; i < r.height.size(); i += 4) ++flagCount[std::min<uint32_t>(uint32_t(r.height[i + 3]), 3)];
            W_CHECK(r.folds == flagCount[3], "fold counter %u, points flagged unsolved %llu", r.folds, (unsigned long long)flagCount[3]);
            W_CHECK(flagCount[2] + flagCount[3] == 0 && r.overflow == 0, "%s: the scatter left %llu points uncovered (%llu unsolved), %u triangles past the loop bound", cam.name,
                    (unsigned long long)(flagCount[2] + flagCount[3]), (unsigned long long)flagCount[3], r.overflow);
            std::vector<double> residuals;
            double worstResidual = 0, worstHeight = 0;
            for (uint32_t i = 0; i < kSamples; ++i)
            {
                const float* q = &r.samples[4 * i];
                const double s = scene.height.s0 * double(1u << samples[4 * i]);
                if (q[2] == 1 || q[2] == 3) continue;  // mesh value (fold edge) or unsolved: no continuous solution stored
                residuals.push_back(q[0]);
                worstResidual = std::max(worstResidual, double(q[0]));
                worstHeight = std::max(worstHeight, std::abs(double(q[1])) / s);
            }
            W_CHECK(worstResidual <= 0.0201, "%s: a solved point's residual is %.4g s_l (> 0.02)", cam.name, worstResidual);
            W_CHECK(worstHeight <= 1e-3, "%s: H differs from water level + h(x0) by %.3g s_l", cam.name, worstHeight);
            std::printf("%s: build over %llu points: polished %llu, mesh value at fold edges %llu, uncovered %llu; residual / s_l at %zu polished samples: median %.2e, "
                        "99%% %.2e, max %.2e; |H - h(x0)| <= %.2e s_l\n",
                        cam.name, (unsigned long long)(r.height.size() / 4), (unsigned long long)flagCount[0], (unsigned long long)flagCount[1],
                        (unsigned long long)(flagCount[2] + flagCount[3]), residuals.size(), percentile(residuals, 0.5), percentile(residuals, 0.99), worstResidual, worstHeight);

            // 2. mips
            checkBounds(r, scene.height.levels);

            // 3. search
            Reference ref{ scene, r.height };
            uint32_t hits = 0, capped = 0, disagree = 0;
            double worstDepth = 0;
            std::vector<double> visits;
            for (uint32_t i = 0; i < kRays; ++i)
            {
                const float* q = &r.rays[4 * i];
                const float* ray = &rays[8 * i];
                const double o[3] = { ray[0], ray[1], ray[2] }, v[3] = { ray[4], ray[5], ray[6] };
                const double tRef = ref.first(o, v);
                visits.push_back(q[2]);
                if (q[0] == 2) { ++capped; continue; }
                const bool gpuHit = q[0] == 1, refHit = tRef >= 0;
                if (gpuHit != refHit)
                {
                    if (disagree < 8)
                        std::printf("  ray %u (elevation %.3f deg): gpu %s t %.6g after %g visits, reference %s t %.6g\n", i, std::asin(v[1]) * 180 / kPi, gpuHit ? "hit" : "miss", q[1], q[2],
                                    refHit ? "hit" : "miss", tRef);
                    ++disagree;
                    continue;
                }
                if (!gpuHit) continue;
                ++hits;
                const double rel = std::abs(q[1] - tRef) / std::max(tRef, 1e-6);
                if (rel > 1e-4 && disagree < 8)
                    std::printf("  ray %u (elevation %.3f deg): gpu t %.7g, reference t %.7g (rel %.2e)\n", i, std::asin(v[1]) * 180 / kPi, q[1], tRef, rel);
                worstDepth = std::max(worstDepth, rel);
            }
            std::printf("%s: %u rays: %u hits, %u misses agreed, %u disagree, %u at the proven visit bound (%u); depth |dt| / t max %.2e; node visits median %.0f, 99%% %.0f, max %.0f\n",
                        cam.name, kRays, hits, kRays - hits - disagree - capped, disagree, capped, 2u * 1024u * scene.height.levels + 32u, worstDepth, percentile(visits, 0.5), percentile(visits, 0.99),
                        percentile(visits, 1));
            W_CHECK(capped == 0, "%s: %u rays reached the proven visit bound", cam.name, capped);
            W_CHECK(disagree == 0, "%s: %u rays disagree on hit or miss", cam.name, disagree);
            W_CHECK(worstDepth <= 1e-4, "%s: depth differs by %.3g t", cam.name, worstDepth);
        }
        const uint32_t errors = gpu.device.drainDebugMessages();
        W_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("ocean height tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
