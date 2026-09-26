// Track W water stage 2 (FEATURES_GAME 1.9; WaterSunMap.ms / .ps, WaterLight.hlsli): the sun-space water map of two
// streams - a flat pool (2 x 2 m at y = 0.4, pure water, IOR 1.333) and a tilted plate (20 degrees, its own medium and
// IOR 1.5) - and waterSunLight at points under, above and beside them, against the exact geometry in double:
//   1. lit exactly where the straight sun ray from the point meets a water surface above it (points within 1 cm of a
//      surface's edge along its plane are boundary cases);
//   2. the light direction = -(the exact Snell refraction of the sunlight at that surface's normal) (within 2e-3; the
//      map stores the normal as octahedral fp16);
//   3. the transmittance = (1 - F(theta_s)) (cos theta_s / cos theta_t) T^d, F the exact unpolarised Fresnel, the cosine
//      ratio the beam's compression across the surface, d the path from the point to the surface along the light
//      direction (relative 3e-3; the medium is fp16); and the energy check: a horizontal floor under the flat pool gets
//      transmittance x cos(floor normal, light direction) = (1 - F) cos theta_s T^d (relative 3e-3);
//   4. with the map absent (constants UNX_NONE) nothing is lit;
//   5. caustics on (a third run): flat water focuses nothing, so where the light reaching the point entered the water more
//      than 4 caustic texels inside a plate the transmittance is unchanged (relative 3e-3), and the caustic grid is 1 at
//      every texel whose value is not an edge's;
//   6. caustics where the water spreads the light (a bowl y = 0.4 + (x^2 + z^2) / 2, sun overhead, spreading about 2x at
//      4 m): 4 m below their surface points (the last slice), the caustic factor (transmittance with / without caustics)
//      equals the exact spreading 1 / |det J| of the landing map (Snell at each entry point, Newton for the entry that lands
//      at the receiver) within 3 % - no gaps between the texels' landing points (the point splat left holes there).
//   unx_test_water_waterlighttests [--no-debug-layer] [--warp]
#include "unx/water/LinearDispatch.h"
#include "unx/water/WaterSunMap.h"

#include "unx/core/File.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
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
ComPtr<ID3D12Resource> hostBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
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
struct V3
{
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }
double fresnel(double cosI, double eta)
{
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double s2 = eta * eta * (1 - cosI * cosI);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2), rs = (eta * cosI - ct) / (eta * cosI + ct), rp = (eta * ct - cosI) / (eta * ct + cosI);
    return 0.5 * (rs * rs + rp * rp);
}
// A water plate: centre, in-plane axes (unit), half sizes, normal (towards the sky), medium.
struct Plate
{
    V3 centre, a, b, n;
    double ha, hb;
    float T[3];
    float ior;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
        }
        Gpu gpu(debugLayer, warp);
        const double tilt = 20.0 * 3.14159265358979323846 / 180.0;
        const Plate plates[2] = {
            { { 0, 0.4, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0, 1.0, { 0.712f, 0.945f, 0.991f }, 1.333f },
            { { 3, 0.4, 0 }, { std::cos(tilt), std::sin(tilt), 0 }, { 0, 0, 1 }, { -std::sin(tilt), std::cos(tilt), 0 }, 0.5, 0.5, { 0.6f, 0.8f, 0.9f }, 1.5f },
        };
        const V3 sun = norm({ 0.3, 0.8, 0.5 });
        // Each plate as two triangles (32 B vertices, normal = the plate's).
        std::vector<std::vector<float>> vertices(2);
        for (int p = 0; p < 2; ++p)
        {
            const Plate& q = plates[p];
            V3 c[4];
            for (int k = 0; k < 4; ++k) c[k] = q.centre + q.a * ((k & 1) ? q.ha : -q.ha) + q.b * ((k & 2) ? q.hb : -q.hb);
            for (int k : { 0, 1, 2, 1, 3, 2 })
                vertices[p].insert(vertices[p].end(), { (float)c[k].x, (float)c[k].y, (float)c[k].z, 1, (float)q.n.x, (float)q.n.y, (float)q.n.z, 0 });
        }
        // Probe points: a grid under and around both plates at two depths, and above the water.
        std::vector<float> points;
        for (double y : { 0.0, 0.25, 0.9 })
            for (double z = -1.3; z <= 1.3; z += 0.05)
                for (double x = -1.5; x <= 3.8; x += 0.05) points.insert(points.end(), { (float)x, (float)y, (float)z, 0 });
        const uint32_t count = uint32_t(points.size() / 4);

        std::vector<uint8_t> result[3];
        std::vector<uint32_t> causticSlice;  // run 2: slice 2 (1 m) of the caustic grid
        uint32_t causticSide = 0;
        const double causticMargin = 0.03;  // 4 caustic texels and more (the grid's extent here is ~5 m over 1024)
        for (int run = 0; run < 3; ++run)
        {
            RenderGraph g(gpu.device);
            std::vector<ComPtr<ID3D12Resource>> keep;
            auto upload = [&](const char* name, const void* data, uint64_t bytes, uint32_t stride = 0) {
                const BufferRef b = g.createBuffer({ name, bytes, stride });
                ComPtr<ID3D12Resource> staging = hostBuffer(gpu.device, bytes, D3D12_HEAP_TYPE_UPLOAD);
                uint8_t* m = nullptr;
                D3D12_RANGE none{ 0, 0 };
                check(staging->Map(0, &none, reinterpret_cast<void**>(&m)), "map");
                std::memcpy(m, data, bytes);
                staging->Unmap(0, nullptr);
                ID3D12Resource* src = staging.Get();
                keep.push_back(staging);
                g.addPass("w.test.upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                          [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, src, 0, bytes); });
                return b;
            };
            std::vector<water::WaterSunStream> streams;
            for (int p = 0; p < 2; ++p)
            {
                water::WaterSunStream w;
                w.stream.vertices = upload("w.test.plate vertices", vertices[p].data(), vertices[p].size() * 4);
                const uint32_t args[4] = { 6, 1, 0, 0 };
                w.stream.drawArgs = upload("w.test.plate args", args, 16);
                w.stream.maxTriangles = 2;
                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                for (size_t v = 0; v < vertices[p].size(); v += 8)
                    for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], vertices[p][v + a]), hi[a] = std::max(hi[a], vertices[p][v + a]);
                w.stream.boundsMin = { lo[0], lo[1], lo[2] };
                w.stream.boundsMax = { hi[0], hi[1], hi[2] };
                std::copy(plates[p].T, plates[p].T + 3, w.transmittance);
                w.ior = plates[p].ior;
                streams.push_back(w);
            }
            water::WaterSunMap map(gpu.device);
            const water::WaterSunMapOutput out = map.record(g, gpu.shaders, 0, streams, { (float)sun.x, (float)sun.y, (float)sun.z });
            W_CHECK(out.texels >= 256, "no sun map");
            const BufferRef pts = upload("w.test.points", points.data(), points.size() * 4, 16);
            const BufferRef res = g.createBuffer({ "w.test.light", uint64_t(count) * 32, 0 });
            ComPtr<ID3D12Resource> rb = hostBuffer(gpu.device, uint64_t(count) * 32, D3D12_HEAP_TYPE_READBACK);
            ID3D12Resource* rbp = rb.Get();
            ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/WaterLightProbe");
            const TextureRef depth = out.depth, normal = out.normal, medium = out.medium, caustics = out.caustics;
            const bool withCaustics = run == 2;
            const BufferRef constants = out.constants;
            const bool absent = run == 1;
            const float sunF[3] = { (float)sun.x, (float)sun.y, (float)sun.z };
            g.addPass("w.test.probe", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(pts, Use::SrvCompute);
                          pb.use(res, Use::UavCompute);
                          pb.use(depth, Use::SrvCompute);
                          pb.use(normal, Use::SrvCompute);
                          pb.use(medium, Use::SrvCompute);
                          pb.use(constants, Use::SrvCompute);
                          pb.use(caustics, Use::SrvCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[12] = { c.srv(pts), c.uav(res), count, 0, c.srv(depth), c.srv(normal), c.srv(medium), absent ? gpu::kNone : c.srv(constants) };
                          std::memcpy(&k[8], sunF, 12);
                          k[11] = withCaustics ? c.srv(caustics) : gpu::kNone;
                          c.cmd->SetPipelineState(probe);
                          c.computeConstants(k, 12);
                          water::dispatchLinear(c.cmd, (count + 63) / 64);
                      });
            ComPtr<ID3D12Resource> crb;
            const uint32_t nc = std::min(out.texels, 1024u), cpitch = (nc * 4 + 255) & ~255u;
            if (withCaustics)
            {
                crb = hostBuffer(gpu.device, uint64_t(cpitch) * nc, D3D12_HEAP_TYPE_READBACK);
                ID3D12Resource* cdst = crb.Get();
                g.addPass("w.test.read caustics", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(caustics, Use::CopySrc);
                              pb.keep();
                          },
                          [=](PassContext& c) {
                              D3D12_TEXTURE_COPY_LOCATION to{ cdst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, from{ c.resource(caustics), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                              to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, nc, nc, 1, cpitch };
                              from.SubresourceIndex = 2;  // slice 2 (mip 0)
                              c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                          });
            }
            g.addPass("w.test.read", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(res, Use::CopySrc);
                          pb.keep();
                      },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(rbp, 0, c.resource(res), 0, uint64_t(count) * 32); });
            g.execute(nullptr);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            const uint8_t* m = nullptr;
            check(rb->Map(0, nullptr, (void**)&m), "map result");
            result[run].assign(m, m + uint64_t(count) * 32);
            rb->Unmap(0, nullptr);
            if (crb)
            {
                const uint8_t* cm = nullptr;
                check(crb->Map(0, nullptr, (void**)&cm), "map caustics");
                causticSlice.resize((size_t)nc * nc);
                for (uint32_t y = 0; y < nc; ++y) std::memcpy(&causticSlice[(size_t)y * nc], cm + (size_t)y * cpitch, nc * 4);
                crb->Unmap(0, nullptr);
                causticSide = nc;
            }
        }

        uint32_t lit = 0, unlit = 0, boundary = 0, wrongLit = 0, wrongUnlit = 0;
        double worstDir = 0, worstT = 0, worstEnergy = 0, worstCaustic = 0;
        uint32_t causticChecked = 0, loggedCaustic = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const V3 X{ points[4 * i], points[4 * i + 1], points[4 * i + 2] };
            // The exact first water surface along the sun ray from X (the one nearest the sun).
            int hit = -1;
            double bestU = -1;
            bool nearEdge = false;
            for (int p = 0; p < 2; ++p)
            {
                const Plate& q = plates[p];
                const double den = dot(sun, q.n);
                if (std::fabs(den) < 1e-12) continue;
                const double u = dot(q.centre - X, q.n) / den;
                if (u <= 0) continue;
                const V3 S = X + sun * u, rel = S - q.centre;
                const double ea = q.ha - std::fabs(dot(rel, q.a)), eb = q.hb - std::fabs(dot(rel, q.b));
                if (std::fabs(ea) < 0.01 || std::fabs(eb) < 0.01) nearEdge = true;
                if (ea >= 0 && eb >= 0 && u > bestU) bestU = u, hit = p;
            }
            float gl[4], gt[4];
            std::memcpy(gl, &result[0][32 * i], 16);
            std::memcpy(gt, &result[0][32 * i + 16], 16);
            uint32_t absentFlag;
            std::memcpy(&absentFlag, &result[1][32 * i + 12], 4);
            W_CHECK(absentFlag == 0, "point %u lit without a map", i);
            uint32_t litWord;
            std::memcpy(&litWord, &result[0][32 * i + 12], 4);
            const bool gpuLit = litWord == 1;
            if (nearEdge) { ++boundary; continue; }
            if ((hit >= 0) != gpuLit)
            {
                (gpuLit ? wrongLit : wrongUnlit)++;
                if (wrongLit + wrongUnlit <= 5) logf("  point (%.2f, %.2f, %.2f): CPU %s, GPU %s\n", X.x, X.y, X.z, hit >= 0 ? "lit" : "unlit", gpuLit ? "lit" : "unlit");
                continue;
            }
            if (hit < 0) { ++unlit; continue; }
            ++lit;
            const Plate& q = plates[hit];
            const double eta = 1.0 / q.ior, cosS = dot(sun, q.n), s2 = eta * eta * (1 - cosS * cosS);
            const V3 t = norm(sun * -eta + q.n * (eta * cosS - std::sqrt(1 - s2)));
            const V3 L = t * -1.0;
            const V3 S = X + sun * bestU;
            const double path = dot(S - X, q.n) / dot(L, q.n), F = fresnel(cosS, eta), cosT = dot(L, q.n);
            worstDir = std::max({ worstDir, std::fabs(gl[0] - L.x), std::fabs(gl[1] - L.y), std::fabs(gl[2] - L.z) });
            for (int c = 0; c < 3; ++c)
            {
                const double want = (1 - F) * (cosS / cosT) * std::pow((double)q.T[c], path);
                worstT = std::max(worstT, std::fabs(gt[c] - want) / want);
                // caustics (third run): where the arriving light entered the plate well inside its edges
                {
                    // The light the GPU reads at X comes, for each slice depth z_k bracketing X's depth h (the surface itself
                    // below the first slice), from the entry point whose refracted ray at depth z_k lands on X's sun column:
                    // S_k = X + sun alpha - t z_k / cos(theta_t), on the plate. All of them well inside the plate.
                    const double h = dot(S - X, q.n), fi = std::log2(std::max(h, 1e-6) / 0.25);
                    std::vector<double> depths;
                    if (fi <= 0) depths = { 0.0, 0.25 };
                    else if (fi >= 4) depths = { 4.0 };
                    else depths = { 0.25 * std::exp2(std::floor(fi)), 0.25 * std::exp2(std::floor(fi) + 1) };
                    bool inside = true;
                    for (double zk : depths)
                    {
                        const V3 back = t * (zk / cosT);  // (t: the refracted direction into the water)
                        const double alpha = (dot(q.centre - X, q.n) + dot(back, q.n)) / dot(sun, q.n);
                        const V3 rel = X + sun * alpha - back - q.centre;
                        inside = inside && q.ha - std::fabs(dot(rel, q.a)) > causticMargin && q.hb - std::fabs(dot(rel, q.b)) > causticMargin;
                    }
                    if (inside)
                    {
                        float gc[4];
                        std::memcpy(gc, &result[2][32 * i + 16], 16);
                        if (std::fabs(gc[c] - want) / want > 3e-3 && loggedCaustic < 6 && c == 1)
                        {
                            ++loggedCaustic;
                            std::printf("  caustic point (%.2f, %.2f, %.2f) plate %d: ratio %.4f, depth below the surface %.3f%c", X.x, X.y, X.z, hit, gc[c] / want,
                                        dot(S - X, q.n), 10);
                        }
                        worstCaustic = std::max(worstCaustic, std::fabs(gc[c] - want) / want);
                        ++causticChecked;
                    }
                }
                if (hit == 0)
                {
                    // energy: the floor's irradiance per E, from the GPU's direction and transmittance
                    const double floorGpu = gt[c] * std::max(0.0, (double)gl[1]), floorWant = (1 - F) * cosS * std::pow((double)q.T[c], path);
                    worstEnergy = std::max(worstEnergy, std::fabs(floorGpu - floorWant) / floorWant);
                }
            }
        }
        std::printf("sun map: %u points lit, %u unlit, %u boundary; misclassified %u lit / %u unlit; light direction within %.2e, transmittance within %.2e (relative)\n", lit,
                    unlit, boundary, wrongLit, wrongUnlit, worstDir, worstT);
        W_CHECK(lit > 1000 && unlit > 1000, "too few points on either side: %u lit, %u unlit", lit, unlit);
        W_CHECK(wrongLit == 0 && wrongUnlit == 0, "%u / %u points misclassified", wrongLit, wrongUnlit);
        W_CHECK(worstDir <= 2e-3, "light direction off by %.3g", worstDir);
        W_CHECK(worstT <= 3e-3, "transmittance off by %.3g relative", worstT);
        W_CHECK(worstEnergy <= 3e-3, "floor irradiance under the flat pool off by %.3g relative", worstEnergy);
        {
            // The grid itself: flat water gives 1 at every texel not on the plates' edges (edges: partial coverage).
            uint32_t exact = 0, close5 = 0, off = 0, nonzero = 0;
            for (size_t k = 0; k < causticSlice.size(); ++k)
            {
                const uint32_t v = causticSlice[k];
                if (!v) continue;
                ++nonzero;
                const double f = v / 65536.0;
                if (std::fabs(f - 1) < 1e-3) ++exact; else if (std::fabs(f - 1) < 0.05) ++close5; else ++off;
            }
            logf("caustic slice 2 (%u^2): %u nonzero, %u within 1e-3 of 1, %u within 5 %%, %u further (the plates' edges)\n", causticSide, nonzero, exact, close5, off);
            W_CHECK(exact > 20 * (close5 + off), "flat caustic grid: %u texels within 1e-3 of 1, %u not (more than the plates' edges)", exact, close5 + off);
        }
        W_CHECK(causticChecked > 1000 && worstCaustic <= 3e-3, "caustics on flat water: %u checks, off by %.3g relative", causticChecked, worstCaustic);
        std::printf("caustics: flat water keeps the transmittance at %u checks within %.2e (relative)%c", causticChecked, worstCaustic, 10);
        W_CHECK(causticSlice.size() > 0, "no caustic slice read");
        // 6. a bowl that spreads the light
        {
            const double kappa = 1.0, y0 = 0.4, ior = 1.333, eta = 1 / ior, zk = 4.0;
            auto height = [&](double x, double z) { return y0 + 0.5 * kappa * (x * x + z * z); };
            auto normalAt = [&](double x, double z) { return norm({ -kappa * x, 1, -kappa * z }); };
            // Landing (x, z) of the overhead sunlight entering at (x, z), zk below the entry along its normal.
            auto landing = [&](double x, double z, double& lx, double& lz) {
                const V3 n = normalAt(x, z), v{ 0, 1, 0 };
                const double cosI = dot(v, n), s2 = eta * eta * (1 - cosI * cosI);
                const V3 t = norm(v * -eta + n * (eta * cosI - std::sqrt(1 - s2)));
                const double cosT = -dot(t, n);
                lx = x + t.x * zk / cosT;
                lz = z + t.z * zk / cosT;
            };
            auto jacobian = [&](double x, double z, double J[4]) {
                const double h = 1e-6;
                double a, b, c, d;
                landing(x + h, z, a, b);
                landing(x - h, z, c, d);
                J[0] = (a - c) / (2 * h), J[2] = (b - d) / (2 * h);
                landing(x, z + h, a, b);
                landing(x, z - h, c, d);
                J[1] = (a - c) / (2 * h), J[3] = (b - d) / (2 * h);
            };
            // The mesh: 128 x 128 quads over [-0.5, 0.5]^2, analytic normals.
            const int cells = 256;
            std::vector<float> bowl;
            auto vertex = [&](int i, int k) {
                const double x = -0.5 + double(i) / cells, z = -0.5 + double(k) / cells;
                const V3 n = normalAt(x, z);
                bowl.insert(bowl.end(), { (float)x, (float)height(x, z), (float)z, 1, (float)n.x, (float)n.y, (float)n.z, 0 });
            };
            for (int k = 0; k < cells; ++k)
                for (int i = 0; i < cells; ++i)
                    for (const auto& c : { std::pair{ 0, 0 }, std::pair{ 0, 1 }, std::pair{ 1, 0 }, std::pair{ 1, 0 }, std::pair{ 0, 1 }, std::pair{ 1, 1 } }) vertex(i + c.first, k + c.second);
            // Receivers over |x|, |z| <= 0.3, zk below their surface point along its normal (the lookup's depth).
            std::vector<float> rec;
            for (double z = -0.3; z <= 0.3001; z += 0.02)
                for (double x = -0.3; x <= 0.3001; x += 0.02)
                {
                    const double ny = normalAt(x, z).y;
                    rec.insert(rec.end(), { (float)x, (float)(height(x, z) - zk / ny), (float)z, 0 });
                }
            const uint32_t nrec = uint32_t(rec.size() / 4);
            std::vector<uint8_t> got[2];
            uint32_t bowlOverflow = 0;
            for (int run = 0; run < 2; ++run)
            {
                RenderGraph g(gpu.device);
                std::vector<ComPtr<ID3D12Resource>> keep;
                auto upload = [&](const char* name, const void* data, uint64_t bytes, uint32_t stride = 0) {
                    const BufferRef b = g.createBuffer({ name, bytes, stride });
                    ComPtr<ID3D12Resource> staging = hostBuffer(gpu.device, bytes, D3D12_HEAP_TYPE_UPLOAD);
                    uint8_t* m = nullptr;
                    D3D12_RANGE none{ 0, 0 };
                    check(staging->Map(0, &none, reinterpret_cast<void**>(&m)), "map");
                    std::memcpy(m, data, bytes);
                    staging->Unmap(0, nullptr);
                    ID3D12Resource* src = staging.Get();
                    keep.push_back(staging);
                    g.addPass("w.test.upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                              [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, src, 0, bytes); });
                    return b;
                };
                water::WaterSunStream w;
                w.stream.vertices = upload("w.test.bowl vertices", bowl.data(), bowl.size() * 4);
                const uint32_t args[4] = { uint32_t(bowl.size() / 8), 1, 0, 0 };
                w.stream.drawArgs = upload("w.test.bowl args", args, 16);
                w.stream.maxTriangles = uint32_t(bowl.size() / 24);
                w.stream.boundsMin = { -0.5f, (float)y0, -0.5f };
                w.stream.boundsMax = { 0.5f, (float)height(0.5, 0.5), 0.5f };
                w.transmittance[0] = w.transmittance[1] = w.transmittance[2] = 1.0f;
                w.ior = (float)ior;
                water::WaterSunMap map(gpu.device);
                const water::WaterSunMapOutput out = map.record(g, gpu.shaders, 0, { w }, { 0, 1, 0 });
                const BufferRef pts = upload("w.test.receivers", rec.data(), rec.size() * 4, 16);
                const BufferRef res = g.createBuffer({ "w.test.bowl light", uint64_t(nrec) * 32, 0 });
                ComPtr<ID3D12Resource> rb = hostBuffer(gpu.device, uint64_t(nrec) * 32 + 16, D3D12_HEAP_TYPE_READBACK);
                ID3D12Resource* rbp = rb.Get();
                ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/WaterLightProbe");
                const TextureRef depth = out.depth, normal = out.normal, medium = out.medium, caustics = out.caustics;
                const BufferRef constants = out.constants, overflow = out.causticOverflow;
                const bool withCaustics = run == 1;
                g.addPass("w.test.bowl probe", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(pts, Use::SrvCompute);
                              pb.use(res, Use::UavCompute);
                              pb.use(depth, Use::SrvCompute);
                              pb.use(normal, Use::SrvCompute);
                              pb.use(medium, Use::SrvCompute);
                              pb.use(constants, Use::SrvCompute);
                              pb.use(caustics, Use::SrvCompute);
                          },
                          [=](PassContext& c) {
                              uint32_t k[12] = { c.srv(pts), c.uav(res), nrec, 0, c.srv(depth), c.srv(normal), c.srv(medium), c.srv(constants) };
                              const float sunF[3] = { 0, 1, 0 };
                              std::memcpy(&k[8], sunF, 12);
                              k[11] = withCaustics ? c.srv(caustics) : gpu::kNone;
                              c.cmd->SetPipelineState(probe);
                              c.computeConstants(k, 12);
                              water::dispatchLinear(c.cmd, (nrec + 63) / 64);
                          });
                g.addPass("w.test.bowl read", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(res, Use::CopySrc);
                              pb.use(overflow, Use::CopySrc);
                              pb.keep();
                          },
                          [=](PassContext& c) {
                              c.cmd->CopyBufferRegion(rbp, 0, c.resource(res), 0, uint64_t(nrec) * 32);
                              c.cmd->CopyBufferRegion(rbp, uint64_t(nrec) * 32, c.resource(overflow), 0, 4);
                          });
                g.execute(nullptr);
                for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
                const uint8_t* m = nullptr;
                check(rb->Map(0, nullptr, (void**)&m), "map bowl");
                got[run].assign(m, m + uint64_t(nrec) * 32);
                if (withCaustics) std::memcpy(&bowlOverflow, m + uint64_t(nrec) * 32, 4);
                rb->Unmap(0, nullptr);
            }
            double worst = 0, lo = 1e9, hi = 0;
            uint32_t checked = 0, logged = 0;
            for (uint32_t i = 0; i < nrec; ++i)
            {
                uint32_t lit0, lit1;
                std::memcpy(&lit0, &got[0][32 * i + 12], 4);
                std::memcpy(&lit1, &got[1][32 * i + 12], 4);
                W_CHECK(lit0 == 1 && lit1 == 1, "bowl receiver %u unlit", i);
                float t0[4], t1[4];
                std::memcpy(t0, &got[0][32 * i + 16], 16);
                std::memcpy(t1, &got[1][32 * i + 16], 16);
                const double factor = t1[1] / t0[1];
                // the entry that lands at the receiver (Newton on the landing map)
                const double gx = rec[4 * i], gz = rec[4 * i + 2];
                double ex = gx * 0.5, ez = gz * 0.5;
                for (int it = 0; it < 30; ++it)
                {
                    double lx, lz, J[4];
                    landing(ex, ez, lx, lz);
                    jacobian(ex, ez, J);
                    const double det = J[0] * J[3] - J[1] * J[2], rx = gx - lx, rz = gz - lz;
                    ex += (J[3] * rx - J[1] * rz) / det;
                    ez += (-J[2] * rx + J[0] * rz) / det;
                }
                double J[4];
                jacobian(ex, ez, J);
                const double want = 1 / std::fabs(J[0] * J[3] - J[1] * J[2]);
                lo = std::min(lo, want), hi = std::max(hi, want);
                const double e = std::fabs(factor / want - 1);
                if (e > 0.03 && logged < 6)
                {
                    ++logged;
                    std::printf("  bowl receiver (%.2f, %.2f): caustic factor %.4f, exact %.4f (entry %.3f, %.3f)\n", gx, gz, factor, want, ex, ez);
                }
                worst = std::max(worst, e);
                ++checked;
            }
            std::printf("caustics in a spreading bowl: %u receivers, exact factor %.3f..%.3f, worst relative error %.3g, %u triangles past the span bound\n", checked, lo, hi, worst, bowlOverflow);
            W_CHECK(hi < 0.6 && worst <= 0.03, "caustics in a spreading bowl: off by %.3g relative (exact factors %.3f..%.3f)", worst, lo, hi);
            W_CHECK(bowlOverflow == 0, "caustics in a spreading bowl: %u triangles past the span bound", bowlOverflow);
        }
        std::printf("energy: a horizontal floor under the flat pool gets (1 - F) cos(theta_s) T^d within %.2e (relative)\n", worstEnergy);
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("water light tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
