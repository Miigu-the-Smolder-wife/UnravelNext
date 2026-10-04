// Round basins W2-R (defect queue 13 (74)). See include/unx/water/RoundPool.h and RoundPool.hlsli.
#include "unx/water/RoundPool.h"

#include "unx/core/Log.h"
#include "unx/render/Frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kTheta = RoundTables::kTheta, kRings = RoundTables::kRings, kOrders = RoundTables::kOrders;
constexpr uint64_t kSamples = uint64_t(kTheta) * kRings;
constexpr uint64_t kTriangles = uint64_t(kTheta) * (1 + 2 * (kRings - 1)), kVertices = 3 * kTriangles;

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "round pool buffer");
    r->SetName(name);
    return r;
}

// J0 and J1 (Numerical Recipes rational approximations, absolute error below 1e-8).
double besselJ0(double x)
{
    const double ax = std::abs(x);
    if (ax < 8.0)
    {
        const double y = x * x;
        const double a = 57568490574.0 + y * (-13362590354.0 + y * (651619640.7 + y * (-11214424.18 + y * (77392.33017 + y * (-184.9052456)))));
        const double b = 57568490411.0 + y * (1029532985.0 + y * (9494680.718 + y * (59272.64853 + y * (267.8532712 + y * 1.0))));
        return a / b;
    }
    const double z = 8.0 / ax, y = z * z, xx = ax - 0.785398164;
    const double a = 1.0 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
    const double b = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934935152e-7)));
    return std::sqrt(0.636619772 / ax) * (std::cos(xx) * a - z * std::sin(xx) * b);
}
double besselJ1(double x)
{
    const double ax = std::abs(x);
    if (ax < 8.0)
    {
        const double y = x * x;
        const double a = x * (72362614232.0 + y * (-7895059235.0 + y * (242396853.1 + y * (-2972611.439 + y * (15704.48260 + y * (-30.16036606))))));
        const double b = 144725228442.0 + y * (2300535178.0 + y * (18583304.74 + y * (99447.43394 + y * (376.9991397 + y * 1.0))));
        return a / b;
    }
    const double z = 8.0 / ax, y = z * z, xx = ax - 2.356194491;
    const double a = 1.0 + y * (0.183105e-2 + y * (-0.3516396496e-4 + y * (0.2457520174e-5 + y * (-0.240337019e-6))));
    const double b = 0.04687499995 + y * (-0.2002690873e-3 + y * (0.8449199096e-5 + y * (-0.88228987e-6 + y * 0.105787412e-6)));
    const double r = std::sqrt(0.636619772 / ax) * (std::cos(xx) * a - z * std::sin(xx) * b);
    return x < 0 ? -r : r;
}
} // namespace

double roundBesselJ(int m, double x)
{
    if (m < 0) fail("roundBesselJ: order %d", m);
    if (m == 0) return besselJ0(x);
    if (m == 1) return besselJ1(x);
    if (x == 0) return 0;
    if (x > m)
    {
        // forward recurrence, stable above the turning point x > m
        double jm1 = besselJ0(x), j = besselJ1(x);
        for (int k = 1; k < m; ++k)
        {
            const double jp1 = (2.0 * k / x) * j - jm1;
            jm1 = j;
            j = jp1;
        }
        return j;
    }
    // Miller's backward recurrence from an even start above m and x, normalised by J0 + 2 sum J_2k = 1
    const int start = 2 * ((m + (int)x + 24) / 2 + 1);
    double jp1 = 0, j = 1e-30, sum = 0, jm = 0;
    for (int k = start; k > 0; --k)
    {
        const double jm1 = (2.0 * k / x) * j - jp1;
        jp1 = j;
        j = jm1;
        if (std::abs(j) > 1e100)
        {
            j *= 1e-100;
            jp1 *= 1e-100;
            sum *= 1e-100;
            jm *= 1e-100;
        }
        if (k - 1 == m) jm = j;
        if (((k - 1) & 1) == 0 && k - 1 > 0) sum += 2 * j;
    }
    sum += j;  // J_0
    return jm / sum;
}

double roundBesselJPrime(int m, double x)
{
    if (m == 0) return -roundBesselJ(1, x);
    return 0.5 * (roundBesselJ(m - 1, x) - roundBesselJ(m + 1, x));
}

std::vector<double> roundDiniRoots(int m, double xMax, uint32_t maxCount)
{
    std::vector<double> roots;
    if (m == 0) roots.push_back(0.0);  // the constant mode (the mean level)
    double x = m == 0 ? 0.5 : std::max(1.0, (double)m);  // J_m' > 0 on (0, its first root) for m >= 1
    const double step = 0.25;
    double f = roundBesselJPrime(m, x);
    while (roots.size() < maxCount && x < xMax)
    {
        const double x1 = x + step, f1 = roundBesselJPrime(m, x1);
        if ((f < 0) != (f1 < 0) && f != 0)
        {
            double a = x, b = x1, fa = f;
            for (int i = 0; i < 60; ++i)
            {
                const double c = 0.5 * (a + b), fc = roundBesselJPrime(m, c);
                if ((fc < 0) == (fa < 0)) { a = c; fa = fc; }
                else b = c;
            }
            const double root = 0.5 * (a + b);
            if (root <= xMax) roots.push_back(root);
        }
        x = x1;
        f = f1;
    }
    return roots;
}

RoundTables roundTables(const RoundPoolDesc& d)
{
    if (!(d.radius > 0) || !(d.depth >= 0) || !(d.gravity > 0) || !(d.tensionOverDensity >= 0) || !(d.viscosity >= 0) || !(d.surfaceFilm == 0 || d.surfaceFilm == 1))
        fail("round pool: invalid description");
    RoundTables t;
    t.radius = d.radius;
    const double R = d.radius, D = d.depth, nu = d.viscosity, film = d.surfaceFilm;
    const double xMax = kPi * kRings;  // the radial Nyquist: k h_r <= pi
    std::vector<double> r(kRings);
    for (uint32_t j = 0; j < kRings; ++j) r[j] = (j + 1) * R / kRings;
    t.modeStart.resize(kOrders);
    t.modeCount.resize(kOrders);
    std::vector<uint32_t> analysisOffset(kOrders), synthesisOffset(kOrders);
    for (uint32_t m = 0; m < kOrders; ++m)
    {
        std::vector<double> roots = roundDiniRoots((int)m, xMax, kRings);
        // least squares over the rings: B[j][n] = J_m(k_n r_j), weights r_j; the normal matrix must be positive definite
        // (an order whose highest modes are not resolved by the rings drops them)
        uint32_t count = (uint32_t)roots.size();
        std::vector<double> B, G, N, L, F;
        bool ok = false;
        while (count > 0 && !ok)
        {
            B.assign((size_t)kRings * count, 0.0);
            for (uint32_t j = 0; j < kRings; ++j)
                for (uint32_t n = 0; n < count; ++n) B[(size_t)j * count + n] = roundBesselJ((int)m, roots[n] / R * r[j]);
            N.assign((size_t)count * count, 0.0);
            for (uint32_t a = 0; a < count; ++a)
                for (uint32_t b = a; b < count; ++b)
                {
                    double s = 0;
                    for (uint32_t j = 0; j < kRings; ++j) s += B[(size_t)j * count + a] * B[(size_t)j * count + b] * r[j];
                    N[(size_t)a * count + b] = N[(size_t)b * count + a] = s;
                }
            // Cholesky N = L L^T
            L.assign((size_t)count * count, 0.0);
            ok = true;
            for (uint32_t i = 0; i < count && ok; ++i)
                for (uint32_t j = 0; j <= i; ++j)
                {
                    double s = N[(size_t)i * count + j];
                    for (uint32_t k = 0; k < j; ++k) s -= L[(size_t)i * count + k] * L[(size_t)j * count + k];
                    if (i == j)
                    {
                        if (!(s > 1e-18 * std::max(1.0, N[(size_t)i * count + i]))) { ok = false; break; }
                        L[(size_t)i * count + i] = std::sqrt(s);
                    }
                    else L[(size_t)i * count + j] = s / L[(size_t)j * count + j];
                }
            if (!ok) --count;
        }
        if (count == 0) fail("round pool: order %u has no resolvable radial mode", m);
        // F = N^-1 B^T W: solve L L^T F = G, G[n][j] = B[j][n] r_j
        G.assign((size_t)count * kRings, 0.0);
        for (uint32_t n = 0; n < count; ++n)
            for (uint32_t j = 0; j < kRings; ++j) G[(size_t)n * kRings + j] = B[(size_t)j * count + n] * r[j];
        F.assign((size_t)count * kRings, 0.0);
        std::vector<double> y(count);
        for (uint32_t j = 0; j < kRings; ++j)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                double s = G[(size_t)i * kRings + j];
                for (uint32_t k = 0; k < i; ++k) s -= L[(size_t)i * count + k] * y[k];
                y[i] = s / L[(size_t)i * count + i];
            }
            for (int i = (int)count - 1; i >= 0; --i)
            {
                double s = y[i];
                for (uint32_t k = i + 1; k < count; ++k) s -= L[(size_t)k * count + i] * F[(size_t)k * kRings + j];
                F[(size_t)i * kRings + j] = s / L[(size_t)i * count + i];
            }
        }
        t.modeStart[m] = t.modeTotal;
        t.modeCount[m] = count;
        analysisOffset[m] = (uint32_t)t.analysis.size();
        synthesisOffset[m] = (uint32_t)t.synthesis.size();
        for (double v : F) t.analysis.push_back((float)v);
        for (uint32_t j = 0; j < kRings; ++j)
            for (uint32_t n = 0; n < count; ++n)
            {
                t.synthesis.push_back((float)B[(size_t)j * count + n]);
                const double k = roots[n] / R;
                t.slope.push_back((float)(k * roundBesselJPrime((int)m, k * r[j])));
            }
        // the modes' frequency, rotation factors and damping (double; the exact linear theory of the cylinder's boundary layers)
        for (uint32_t n = 0; n < count; ++n)
        {
            const double x = roots[n], k = x / R;
            t.k.push_back(k);
            if (k == 0)
            {
                for (int i = 0; i < 4; ++i) t.modes.push_back(0.0f);
                continue;
            }
            const double K = D > 0 ? k * std::tanh(k * D) : k, Gk = (double)d.gravity + (double)d.tensionOverDensity * k * k, w = std::sqrt(K * Gk);
            const double Jm = roundBesselJ((int)m, x), A = m == 0 ? 2 * kPi : kPi;
            const double I2 = 0.5 * R * R * (1 - (double)m * m / (x * x)) * Jm * Jm;  // int_0^R J_m(k r)^2 r dr at a Dini root
            double C, S, floorD, surfD;
            if (D > 0)
            {
                const double s2 = std::sinh(2 * k * D);
                C = 0.5 * D + s2 / (4 * k);
                S = -0.5 * D + s2 / (4 * k);
                floorD = A * k * k * I2;
                surfD = floorD * std::cosh(k * D) * std::cosh(k * D);
            }
            else
            {
                C = S = 0.5 / k;
                floorD = 0;
                surfD = A * k * k * I2;
            }
            const double E = A * k * k * I2 * (C + S);  // int |grad Phi|^2 over the water
            const double wallD = R * Jm * Jm * A * ((double)m * m / (R * R) * C + k * k * S);
            const double delta = 2 * nu * k * k + (E > 0 ? std::sqrt(0.5 * nu * w) * (floorD + wallD + film * surfD) / (2 * E) : 0.0);
            t.modes.push_back((float)w);
            t.modes.push_back((float)(K / w));
            t.modes.push_back((float)(w / K));
            t.modes.push_back((float)delta);
        }
        t.modeTotal += count;
    }
    // orders: (start, count, analysis offset, synthesis offset) as uint4
    std::vector<uint32_t>& ms = t.modeStart;
    (void)ms;
    t.analysis.shrink_to_fit();
    // pack the offsets into modeStart/modeCount's siblings through the class (the GPU table is built by the pool)
    t.modeStart.insert(t.modeStart.end(), analysisOffset.begin(), analysisOffset.end());
    t.modeStart.insert(t.modeStart.end(), synthesisOffset.begin(), synthesisOffset.end());
    return t;
}

RoundPool::RoundPool(Device& device, ShaderLibrary& shaders, const RoundPoolDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    m_topologyId = allocateTriangleStreamTopologyId();
    if (!desc.maxSources || !desc.framesInFlight) fail("round pool: invalid description");
    m_tables = roundTables(desc);
    const RoundTables& t = m_tables;
    const uint64_t modeBytes = uint64_t(t.modeTotal) * 16;
    m_modes = makeBuffer(device, modeBytes, D3D12_HEAP_TYPE_DEFAULT, L"round pool modes");
    m_increments = makeBuffer(device, modeBytes, D3D12_HEAP_TYPE_DEFAULT, L"round pool increments");
    m_accum = makeBuffer(device, kSamples * 8, D3D12_HEAP_TYPE_DEFAULT, L"round pool sources");
    m_spectrum = makeBuffer(device, uint64_t(kRings) * kOrders * 48, D3D12_HEAP_TYPE_DEFAULT, L"round pool spectrum");
    m_previous = makeBuffer(device, (kSamples + 1) * 4, D3D12_HEAP_TYPE_DEFAULT, L"round pool previous eta");
    m_twiddles = makeBuffer(device, kTheta * 4, D3D12_HEAP_TYPE_DEFAULT, L"round pool twiddles");
    m_table = makeBuffer(device, modeBytes, D3D12_HEAP_TYPE_DEFAULT, L"round pool mode table");
    m_analysis = makeBuffer(device, t.analysis.size() * 4, D3D12_HEAP_TYPE_DEFAULT, L"round pool analysis");
    m_synthesis = makeBuffer(device, t.synthesis.size() * 4, D3D12_HEAP_TYPE_DEFAULT, L"round pool synthesis");
    m_slope = makeBuffer(device, t.slope.size() * 4, D3D12_HEAP_TYPE_DEFAULT, L"round pool slope");
    m_orders = makeBuffer(device, kOrders * 16, D3D12_HEAP_TYPE_DEFAULT, L"round pool orders");
    m_centre = makeBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, L"round pool centre");
    // one upload of every table: twiddles, modes, orders, analysis, synthesis, slope
    {
        std::vector<uint8_t> blob;
        auto append = [&](const void* p, size_t bytes) { const size_t at = blob.size(); blob.resize(at + bytes); std::memcpy(blob.data() + at, p, bytes); return at; };
        std::vector<float> tw(kTheta);
        for (uint32_t j = 0; j < kTheta / 2; ++j) { tw[2 * j] = float(std::cos(2 * kPi * j / kTheta)); tw[2 * j + 1] = float(std::sin(2 * kPi * j / kTheta)); }
        std::vector<uint32_t> orders(kOrders * 4);
        for (uint32_t m = 0; m < kOrders; ++m)
        {
            orders[4 * m] = t.modeStart[m];
            orders[4 * m + 1] = t.modeCount[m];
            orders[4 * m + 2] = t.modeStart[kOrders + m];      // analysis offset
            orders[4 * m + 3] = t.modeStart[2 * kOrders + m];  // synthesis offset
        }
        append(tw.data(), tw.size() * 4);
        append(t.modes.data(), t.modes.size() * 4);
        append(orders.data(), orders.size() * 4);
        append(t.analysis.data(), t.analysis.size() * 4);
        append(t.synthesis.data(), t.synthesis.size() * 4);
        append(t.slope.data(), t.slope.size() * 4);
        m_tableUpload = makeBuffer(device, blob.size(), D3D12_HEAP_TYPE_UPLOAD, L"round pool tables upload");
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_tableUpload->Map(0, &none, &mapped), "map round pool tables");
        std::memcpy(mapped, blob.data(), blob.size());
        m_tableUpload->Unmap(0, nullptr);
    }
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTheta;
    td.Height = kRings;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_output)), "round pool output");
    m_output->SetName(L"round pool field");
    for (uint32_t s = 0; s < desc.framesInFlight; ++s)
    {
        m_sourceUpload.push_back(makeBuffer(device, uint64_t(desc.maxSources) * 32, D3D12_HEAP_TYPE_UPLOAD, L"round pool sources upload"));
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_sourceUpload.back()->Map(0, &none, (void**)&mapped), "map round pool sources upload");
        m_sourceMapped.push_back(mapped);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(desc.maxSources) * 8;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_sourceSrv.push_back(device.descriptors().allocateResource());
        device.d3d()->CreateShaderResourceView(m_sourceUpload.back().Get(), &sd, device.descriptors().resourceCpu(m_sourceSrv.back()));
    }
}
RoundPool::~RoundPool()
{
    for (auto& u : m_sourceUpload) u->Unmap(0, nullptr);
    for (const ComPtr<ID3D12Resource>& r : { m_modes, m_increments, m_accum, m_spectrum, m_previous, m_twiddles, m_table, m_analysis, m_synthesis, m_slope, m_orders,
                                              m_centre, m_output, m_tableUpload })
        if (r) m_device.deferRelease(r);
    for (auto& u : m_sourceUpload) m_device.deferRelease(u);
    for (uint32_t srv : m_sourceSrv) m_device.descriptors().freeResource(srv);
}

bool RoundPool::contains(const RoundPoolDesc& desc, const RoundPoolPlacement& placement, double x, double z)
{
    const double dx = x - placement.centre[0], dz = z - placement.centre[2];
    return std::sqrt(dx * dx + dz * dz) <= desc.radius * (1 + 0.5 / kRings);
}

void RoundPool::evolve(RenderGraph& g, const Refs& r, float dt, uint32_t sourceSrv, uint32_t sourceCount, float meanShift)
{
    const bool increment = sourceCount > 0 || meanShift != 0;
    const RoundPoolDesc d = m_desc;
    const Refs refs = r;
    auto constants = [=](PassContext& c) {
        uint32_t k[20] = { c.uav(refs.modes), c.uav(refs.increments), c.uav(refs.spectrum), c.uav(refs.accum),
                           0, c.uav(refs.previous), c.srv(refs.twiddles), c.srv(refs.orders),
                           sourceSrv, sourceCount, 0, 0,
                           c.srv(refs.table), c.srv(refs.analysis), c.srv(refs.synthesis), c.srv(refs.slope),
                           c.uav(refs.field), c.uav(refs.centre), increment ? 1u : 0u, 0 };
        std::memcpy(&k[4], &dt, 4);
        std::memcpy(&k[10], &d.radius, 4);
        std::memcpy(&k[11], &d.depth, 4);
        std::memcpy(&k[19], &meanShift, 4);
        c.computeConstants(k, 20);
    };
    auto uses = [&](PassBuilder& pb) {
        for (BufferRef b : { refs.modes, refs.increments, refs.spectrum, refs.accum, refs.previous, refs.centre }) pb.use(b, Use::UavCompute);
        for (BufferRef b : { refs.twiddles, refs.orders, refs.table, refs.analysis, refs.synthesis, refs.slope }) pb.use(b, Use::SrvCompute);
        pb.use(refs.field, Use::UavCompute);
    };
    auto pass = [&](const char* name, const char* kernel, uint32_t groups) {
        ID3D12PipelineState* pso = m_shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics, uses, [=](PassContext& c) { c.cmd->SetPipelineState(pso); constants(c); c.cmd->Dispatch(groups, 1, 1); });
    };
    if (sourceCount) pass("round sources", "Passes/Water/RoundSplat", sourceCount);
    if (increment)
    {
        pass("round forward rings", "Passes/Water/RoundRows", kRings);
        pass("round analysis", "Passes/Water/RoundAnalysis", kOrders);
    }
    pass("round evolve", "Passes/Water/RoundEvolve", (m_tables.modeTotal + 255) / 256);
    pass("round synthesis", "Passes/Water/RoundSynthesis", kOrders);
    pass("round inverse rings", "Passes/Water/RoundColumns", kRings);
}

RoundPoolOutput RoundPool::record(RenderGraph& g, uint64_t frame, const RoundPoolPlacement& placement, double time, float frameDt,
                                  const std::vector<RoundPoolSource>& sources)
{
    if (!(frameDt >= 0)) fail("round pool: negative frame time");
    if (sources.size() > m_desc.maxSources) fail("round pool: %zu sources exceed the capacity %u", sources.size(), m_desc.maxSources);
    if (m_started && !(time >= m_time)) fail("round pool: time went back (%.9g after %.9g)", time, m_time);
    const uint32_t slot = uint32_t(frame % m_desc.framesInFlight);
    const double c = std::cos(double(placement.yaw)), s = std::sin(double(placement.yaw));
    uint8_t* mapped = m_sourceMapped[slot];
    double volume = 0;
    for (size_t i = 0; i < sources.size(); ++i)
    {
        const double dx = sources[i].x - placement.centre[0], dz = sources[i].z - placement.centre[2];
        const double lx = dx * c - dz * s, lz = dx * s + dz * c;
        if (!(std::sqrt(lx * lx + lz * lz) <= m_desc.radius * (1 + 0.5 / kRings)))
            fail("round pool: source %zu at (%.6g, %.6g) lies outside the basin (local %.6g, %.6g, R %g)", i, sources[i].x, sources[i].z, lx, lz, m_desc.radius);
        const float v[8] = { (float)lx, (float)lz, sources[i].radius, sources[i].impulse, sources[i].volume, 0, 0, 0 };
        std::memcpy(mapped + 32 * i, v, 32);
        volume += sources[i].volume;
    }
    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    Refs refs;
    refs.modes = import(m_modes.Get(), "round pool modes");
    refs.increments = import(m_increments.Get(), "round pool increments");
    refs.accum = import(m_accum.Get(), "round pool sources");
    refs.spectrum = import(m_spectrum.Get(), "round pool spectrum");
    refs.previous = import(m_previous.Get(), "round pool previous eta");
    refs.twiddles = import(m_twiddles.Get(), "round pool twiddles");
    refs.table = import(m_table.Get(), "round pool mode table");
    refs.analysis = import(m_analysis.Get(), "round pool analysis");
    refs.synthesis = import(m_synthesis.Get(), "round pool synthesis");
    refs.slope = import(m_slope.Get(), "round pool slope");
    refs.orders = import(m_orders.Get(), "round pool orders");
    refs.centre = import(m_centre.Get(), "round pool centre");
    refs.field = g.importTexture(m_output.Get(), TextureDesc{ "round pool field", kTheta, kRings, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT }, D3D12_BARRIER_LAYOUT_COMMON);
    const Refs r = refs;
    if (!m_initialised)
    {
        const RoundTables& t = m_tables;
        ID3D12Resource* upload = m_tableUpload.Get();
        const uint64_t twBytes = kTheta * 4, modeBytes = t.modes.size() * 4, orderBytes = kOrders * 16, anBytes = t.analysis.size() * 4, syBytes = t.synthesis.size() * 4,
                       slBytes = t.slope.size() * 4;
        g.addPass("round tables", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      for (BufferRef b : { r.twiddles, r.table, r.orders, r.analysis, r.synthesis, r.slope }) pb.use(b, Use::CopyDst);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      uint64_t at = 0;
                      c.cmd->CopyBufferRegion(c.resource(r.twiddles), 0, upload, at, twBytes); at += twBytes;
                      c.cmd->CopyBufferRegion(c.resource(r.table), 0, upload, at, modeBytes); at += modeBytes;
                      c.cmd->CopyBufferRegion(c.resource(r.orders), 0, upload, at, orderBytes); at += orderBytes;
                      c.cmd->CopyBufferRegion(c.resource(r.analysis), 0, upload, at, anBytes); at += anBytes;
                      c.cmd->CopyBufferRegion(c.resource(r.synthesis), 0, upload, at, syBytes); at += syBytes;
                      c.cmd->CopyBufferRegion(c.resource(r.slope), 0, upload, at, slBytes);
                  });
        ID3D12PipelineState* clear = m_shaders.compute("Passes/Water/RoundClear");
        const uint32_t groups = (uint32_t)((std::max<uint64_t>(kSamples, t.modeTotal) + 255) / 256);
        const RoundPoolDesc d = m_desc;
        g.addPass("round clear", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      for (BufferRef b : { r.modes, r.accum, r.previous, r.centre }) pb.use(b, Use::UavCompute);
                      pb.use(r.orders, Use::SrvCompute);
                      pb.use(r.field, Use::UavCompute);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      uint32_t k[20] = { c.uav(r.modes), 0, 0, c.uav(r.accum), 0, c.uav(r.previous), 0, c.srv(r.orders), 0, 0, 0, 0, 0, 0, 0, 0, c.uav(r.field), c.uav(r.centre), 0, 0 };
                      std::memcpy(&k[10], &d.radius, 4);
                      c.cmd->SetPipelineState(clear);
                      c.computeConstants(k, 20);
                      c.cmd->Dispatch(groups, 1, 1);
                  });
        m_initialised = true;
    }
    const double area = kPi * double(m_desc.radius) * m_desc.radius;
    double gap = m_started ? time - m_time : 0.0;
    if (gap > double(frameDt) * (1 + 1e-6) + 1e-9)
    {
        evolve(g, refs, float(gap - double(frameDt)), 0, 0, 0.0f);
        evolve(g, refs, float(frameDt), m_sourceSrv[slot], uint32_t(sources.size()), float(volume / area));
    }
    else evolve(g, refs, float(gap), m_sourceSrv[slot], uint32_t(sources.size()), float(volume / area));
    m_time = time;
    m_started = true;

    const BufferRef vertices = g.createBuffer({ "round pool surface vertices", kVertices * 32, 0 }), velocities = g.createBuffer({ "round pool surface velocities", kVertices * 16, 0 }),
                    draw = g.createBuffer({ "round pool surface draw", 16, 0 });
    const float centreXZ[2] = { float(placement.centre[0]), float(placement.centre[2]) }, level = float(placement.centre[1]), invDt = frameDt > 0 ? 1.0f / frameDt : 0.0f;
    const float axis[4] = { float(c), float(-s), float(s), float(c) };  // ax = (cos, -sin), az = (sin, cos) in world (x, z)
    const float radius = m_desc.radius;
    ID3D12PipelineState* mesh = m_shaders.compute("Passes/Water/RoundMesh");
    g.addPass("round surface", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(r.field, Use::SrvCompute); pb.use(r.previous, Use::SrvCompute); pb.use(r.centre, Use::SrvCompute);
                  pb.use(vertices, Use::UavCompute); pb.use(velocities, Use::UavCompute); pb.use(draw, Use::UavCompute);
              },
              [=](PassContext& c2) {
                  uint32_t k[16] = { c2.srv(r.field), c2.srv(r.previous), c2.uav(vertices), c2.uav(velocities), c2.uav(draw), 0, 0, c2.srv(r.centre), 0, 0, 0, 0 };
                  std::memcpy(&k[5], &invDt, 4);
                  std::memcpy(&k[6], &level, 4);
                  std::memcpy(&k[8], centreXZ, 8);
                  std::memcpy(&k[10], &radius, 4);
                  std::memcpy(&k[12], axis, 16);
                  c2.cmd->SetPipelineState(mesh);
                  c2.computeConstants(k, 16);
                  c2.cmd->Dispatch(kTheta / 8, (kRings - 1 + 7) / 8, 1);
              });
    RoundPoolOutput out;
    out.field = refs.field;
    out.centre = refs.centre;
    TriangleStream& st = out.stream;
    st.vertices = vertices;
    st.velocities = velocities;
    st.drawArgs = draw;
    st.maxTriangles = uint32_t(kTriangles);
    st.layer = 1;
    st.fixedTopologyId = m_topologyId; // RoundMesh's fixed fan and ring quads are always active
    st.knownTriangleCount = st.maxTriangles;
    const double vertical = m_desc.depth > 0 ? m_desc.depth : 1.0;
    st.boundsMin = { float(placement.centre[0] - radius), float(placement.centre[1] - vertical), float(placement.centre[2] - radius) };
    st.boundsMax = { float(placement.centre[0] + radius), float(placement.centre[1] + vertical), float(placement.centre[2] + radius) };
    return out;
}
} // namespace unx::water
