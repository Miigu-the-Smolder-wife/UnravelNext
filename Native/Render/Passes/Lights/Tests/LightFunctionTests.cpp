// Track E light function correctness (A8; FEATURES_GAME 12).
//   - parseIes: LM-63 header, TILT=NONE only, type C only, multiplier x ballast, file order (per horizontal angle) to
//     [v * H + h], peak normalisation, every accepted symmetry (0; 0..90; 0..180; 0..360; 90..270), rejections;
//   - mipChain: each level keeps the image's mean exactly (area-weighted box at odd sizes too);
//   - time functions (CPU): looping and held intensity / colour keys, flicker bounded, near 1 on average, continuous,
//     deterministic per seed; rotation turns the profile about the forward axis;
//   - GPU = CPU: LightFunction.hlsli against lights::evaluate for 6 lights (IES of three symmetries, a rotating
//     cookie, a gobo, a light with time functions only) and a light without a function, 4096 directions x times,
//     through the frame track (tracks::lightFunctions: table and images uploaded in the graph); the only differences
//     allowed beyond float and half-float rounding are at discontinuities (the cookie frame, the IES vertical range),
//     recognised by a CPU evaluation 1e-4 rad away agreeing with the GPU;
//   - the cookie's mip level follows the receiver footprint: 1-texel stripes seen through an 8-texel footprint give
//     their mean 0.5 (no aliasing), at footprint 0 the sharp stripes;
//   - a second frame reuses the table and images (same results), a changed image is re-uploaded, clearing every
//     function leaves FrameResources::lightFunctions invalid.
//   unx_test_lights_lightfunctiontests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/lights/LightFunctions.h"
#include "unx/render/Tracks.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
constexpr float kPi = 3.14159265358979f;

std::string iesText(const char* tilt, int type, double multiplier, const std::vector<float>& vertical, const std::vector<float>& horizontal,
                    const std::vector<float>& candelaPerHorizontal /* H x V, file order */, double ballast = 1)
{
    std::string s = "IESNA:LM-63-2002\r\n[TEST] light function\r\n[MANUFAC] none\r\n";
    s += std::string("TILT=") + tilt + "\r\n";
    char line[256];
    std::snprintf(line, sizeof line, "1 1000 %g %zu %zu %d 2 0.1 0.1 0.05\r\n%g 1 30\r\n", multiplier, vertical.size(), horizontal.size(), type, ballast);
    s += line;
    for (float a : vertical) s += std::to_string(a) + " ";
    s += "\r\n";
    for (float a : horizontal) s += std::to_string(a) + " ";
    s += "\r\n";
    for (size_t i = 0; i < candelaPerHorizontal.size(); ++i) s += std::to_string(candelaPerHorizontal[i]) + ((i + 1) % vertical.size() ? " " : "\r\n");
    return s;
}
bool throws(const std::string& text)
{
    try
    {
        lights::parseIes(text);
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}
float3 norm(float3 a)
{
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return { a.x / l, a.y / l, a.z / l };
}
float3 crossv(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float maxAbs(float3 a, float3 b) { return std::max({ std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z) }); }

// An IES profile of a given symmetry with a smooth, asymmetric candela field (values in the file's order).
lights::LightFunction iesLight(const std::vector<float>& horizontal, float vMax, float scale)
{
    std::vector<float> vertical;
    for (float v = 0; v <= vMax + 1e-3f; v += 5) vertical.push_back(v);
    std::vector<float> c;
    for (float h : horizontal)
        for (float v : vertical) c.push_back(scale * (100 + 80 * std::cos(v * kPi / 180) + 30 * std::sin(h * kPi / 180 * 0.5f) * std::sin(v * kPi / 90)));
    lights::LightFunction f;
    f.profile = lights::Profile::Ies;
    f.ies = lights::parseIes(iesText("NONE", 1, 1, vertical, horizontal, c));
    return f;
}
lights::Image smoothImage(uint32_t w, uint32_t h, float phase)
{
    lights::Image im;
    im.width = w;
    im.height = h;
    im.rgb.resize((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            float* p = im.rgb.data() + ((size_t)y * w + x) * 3;
            p[0] = 0.5f + 0.4f * std::sin(2 * kPi * x / 16 + phase);
            p[1] = 0.5f + 0.4f * std::cos(2 * kPi * y / 12 + phase);
            p[2] = 0.3f + 0.2f * std::sin(2 * kPi * (x + y) / 24);
        }
    return im;
}

struct Query
{
    float4 dir;      // dir, time
    float4 forward;  // forward, footprint
    float4 right;    // right, light (uint bits)
};

std::vector<float4> gpuEvaluate(TestFrame& tf, const std::vector<Query>& q, bool* tableValid = nullptr)
{
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        tracks::lightFunctions(fc);
        const BufferRef table = fc.resources.lightFunctions;
        if (tableValid) *tableValid = table.valid();
        const BufferRef in = tf.uploadBuffer(fc, q.data(), q.size() * sizeof(Query), 16, "lights.test.queries");
        const BufferRef res = fc.graph.createBuffer(BufferDesc{ "lights.test.results", q.size() * 16, 16 });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Lights/Tests/LightFunctionProbe");
        const uint32_t n = (uint32_t)q.size();
        fc.graph.addPass("lights.test.probe", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(in, Use::SrvCompute);
                             b.use(res, Use::UavCompute);
                             if (table.valid()) b.use(table, Use::SrvCompute);
                         },
                         [=](PassContext& ctx) {
                             const uint32_t k[4] = { ctx.srv(in), ctx.uav(res), n, table.valid() ? ctx.srv(table) : 0xFFFFFFFFu };
                             ctx.cmd->SetPipelineState(pso);
                             ctx.computeConstants(k, 4);
                             ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, q.size() * 16);
    });
    std::vector<float4> r(q.size());
    std::memcpy(r.data(), out->data(), q.size() * 16);
    return r;
}
uint32_t lightOf(const Query& q)
{
    uint32_t l;
    std::memcpy(&l, &q.right.w, 4);
    return l;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown argument %s", a.c_str());
        }

        // ---- parseIes ----
        {
            // 3 vertical x 2 horizontal, file order per horizontal angle; multiplier 2, ballast 0.5
            const lights::IesProfile p = lights::parseIes(iesText("NONE", 1, 2, { 0, 45, 90 }, { 0, 180 }, { 10, 20, 30, 40, 50, 60 }, 0.5));
            S_CHECK(p.vertical.size() == 3 && p.horizontal.size() == 2 && p.peak == 60, "IES header / peak %g", p.peak);
            const float expect[6] = { 10, 40, 20, 50, 30, 60 };  // [v * H + h]
            for (int i = 0; i < 6; ++i) S_CHECK(std::abs(p.values[i] - expect[i] / 60) <= 1e-7f, "IES value %d = %g", i, p.values[i]);
            S_CHECK(throws(iesText("INCLUDE", 1, 1, { 0, 90 }, { 0 }, { 1, 2 })), "TILT=INCLUDE accepted");
            S_CHECK(throws(iesText("NONE", 2, 1, { 0, 90 }, { 0 }, { 1, 2 })), "type B accepted");
            S_CHECK(throws(iesText("NONE", 1, 1, { 0, 90 }, { 0, 120 }, { 1, 2, 3, 4 })), "horizontal 0..120 accepted");
            S_CHECK(throws(iesText("NONE", 1, 1, { 0, 90 }, { 0 }, { 1, 2 }) + " 7"), "trailing numbers accepted");
            S_CHECK(throws(iesText("NONE", 1, 1, { 90, 0 }, { 0 }, { 1, 2 })), "descending vertical angles accepted");
            S_CHECK(throws(iesText("NONE", 1, 1, { 0, 90 }, { 0 }, { 0, 0 })), "an all-zero profile accepted");
            for (const std::vector<float>& h : std::vector<std::vector<float>>{ { 0 }, { 0, 45, 90 }, { 0, 90, 180 }, { 0, 90, 180, 270, 360 }, { 90, 180, 270 } })
            {
                std::vector<float> c(h.size() * 2, 5.0f);
                S_CHECK(!throws(iesText("NONE", 1, 1, { 0, 90 }, h, c)), "symmetry %g..%g rejected", h.front(), h.back());
            }
            std::printf("IES: LM-63 parsed (multiplier x ballast, file order transposed, peak 1), 5 symmetries accepted, 6 invalid files rejected\n");
        }

        // ---- mipChain ----
        for (const auto& [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{ { 5, 3 }, { 7, 1 }, { 64, 32 }, { 9, 9 } })
        {
            const lights::Image im = smoothImage(w, h, 0.3f);
            const std::vector<lights::Image> chain = lights::mipChain(im);
            S_CHECK(chain.back().width == 1 && chain.back().height == 1, "%ux%u chain does not end at 1 x 1", w, h);
            double mean0[3] = {};
            for (size_t i = 0; i < im.rgb.size(); ++i) mean0[i % 3] += im.rgb[i] / ((double)w * h);
            for (const lights::Image& m : chain)
            {
                double mean[3] = {};
                for (size_t i = 0; i < m.rgb.size(); ++i) mean[i % 3] += m.rgb[i] / ((double)m.width * m.height);
                for (int c = 0; c < 3; ++c) S_CHECK(std::abs(mean[c] - mean0[c]) <= 1e-6, "%ux%u level %ux%u mean %g vs %g", w, h, m.width, m.height, mean[c], mean0[c]);
            }
        }
        std::printf("mips: box chains of 5x3, 7x1, 64x32, 9x9 keep the image mean at every level (1e-6)\n");

        // ---- time functions (CPU) ----
        {
            lights::LightFunction f;
            f.intensityKeys = { { 0, 1 }, { 1, 3 }, { 2, 1 } };
            f.intensityPeriod = 2;
            const float3 fw{ 0, 0, 1 }, rt{ 1, 0, 0 }, d{ 0, 0, 1 };
            S_CHECK(std::abs(lights::evaluate(f, fw, rt, d, 0.5f).x - 2) <= 1e-6f && std::abs(lights::evaluate(f, fw, rt, d, 2.5f).x - 2) <= 1e-5f &&
                        std::abs(lights::evaluate(f, fw, rt, d, 1).x - 3) <= 1e-6f,
                    "intensity keys");
            f.intensityPeriod = 0;
            S_CHECK(lights::evaluate(f, fw, rt, d, 5).x == 1, "intensity held after the last key");
            f.intensityKeys.clear();
            f.colorKeys = { { 0, 1, 0, 0 }, { 4, 0, 0, 1 } };
            const float3 c = lights::evaluate(f, fw, rt, d, 1);
            S_CHECK(std::abs(c.x - 0.75f) <= 1e-6f && c.y == 0 && std::abs(c.z - 0.25f) <= 1e-6f, "colour keys (%g %g %g)", c.x, c.y, c.z);
            f.colorKeys.clear();
            f.flickerDepth = 0.5f;
            f.flickerFrequency = 8;
            f.flickerOctaves = 3;
            double sum = 0, lo = 1e9, hi = -1e9, step = 0;
            float prev = lights::evaluate(f, fw, rt, d, 0).x;
            const int n = 200000;
            for (int i = 0; i < n; ++i)
            {
                const float v = lights::evaluate(f, fw, rt, d, i * 5e-4f).x;
                sum += v;
                lo = std::min<double>(lo, v);
                hi = std::max<double>(hi, v);
                step = std::max<double>(step, std::abs(v - prev));
                prev = v;
            }
            // continuity: |df/dt| <= depth x sum_o a_o 1.5 f 2^o / sum a_o (smoothstep slope 1.5 x 2 of the value span)
            const double slope = 0.5 * (1.5 * 2 * 8 * (1 + 0.5 * 2 + 0.25 * 4)) / 1.75;
            S_CHECK(lo >= 0 && hi <= 1.5 && std::abs(sum / n - 1) <= 0.05 && step <= slope * 5e-4 * 1.01, "flicker: min %g max %g mean %g max step %g", lo, hi, sum / n, step);
            lights::LightFunction g = f;
            S_CHECK(lights::evaluate(g, fw, rt, d, 3.3f).x == lights::evaluate(f, fw, rt, d, 3.3f).x, "flicker not deterministic");
            g.flickerSeed = 2;
            S_CHECK(lights::evaluate(g, fw, rt, d, 3.3f).x != lights::evaluate(f, fw, rt, d, 3.3f).x, "flicker seeds give the same signal");
            std::printf("time: looping and held keys, colour keys exact; flicker in [%.3f, %.3f], mean %.4f over 100 s, max step %.4f per 0.5 ms (bound %.4f)\n", lo, hi,
                        sum / n, step, slope * 5e-4);

            lights::LightFunction r = iesLight({ 0, 30, 60, 90, 120, 150, 180 }, 90, 1);
            r.rotationSpeed = 0.7f;
            float worst = 0;
            std::mt19937 rng(7);
            std::uniform_real_distribution<float> u(-1, 1);
            for (int i = 0; i < 1000; ++i)
            {
                const float3 dir = norm({ u(rng), u(rng), std::abs(u(rng)) + 0.1f });
                const float t = 1.3f;
                const float a = -0.7f * t;  // the direction turned back by the profile's rotation
                const float3 back = { dir.x * std::cos(a) - dir.y * std::sin(a), dir.x * std::sin(a) + dir.y * std::cos(a), dir.z };
                lights::LightFunction still = r;
                still.rotationSpeed = 0;
                worst = std::max(worst, maxAbs(lights::evaluate(r, fw, rt, dir, t), lights::evaluate(still, fw, rt, back, t)));
            }
            S_CHECK(worst <= 2e-5f, "rotation: %g", worst);
            std::printf("rotation: the profile at time t = the still profile at the direction turned back by speed x t (%.1e)\n", worst);
        }

        // ---- GPU = CPU through the frame track ----
        TestFrame tf(debugLayer);
        lights::LightFunctions& set = lights::lightFunctions(tf.trackState);
        std::vector<lights::LightFunction> fs(7);
        fs[0] = iesLight({ 0, 22.5f, 45, 67.5f, 90, 112.5f, 135, 157.5f, 180 }, 90, 3);  // bilateral, lower hemisphere only
        fs[1] = iesLight({ 0 }, 180, 1);                                                  // rotationally symmetric
        fs[2] = iesLight({ 90, 135, 180, 225, 270 }, 120, 2);                               // 90..270
        fs[2].rotationSpeed = 0.4f;
        fs[3].profile = lights::Profile::Cookie;
        fs[3].image = smoothImage(64, 48, 0.2f);
        fs[3].tanX = 0.8f;
        fs[3].tanY = 0.6f;
        fs[3].rotationSpeed = 0.3f;
        fs[3].rotationPhase = 0.5f;
        fs[4].profile = lights::Profile::Gobo;
        fs[4].image = smoothImage(96, 48, 1.1f);
        fs[5].intensityKeys = { { 0, 0.5f }, { 1.5f, 2 }, { 3, 0.5f } };
        fs[5].intensityPeriod = 3;
        fs[5].colorKeys = { { 0, 1, 0.8f, 0.6f }, { 2, 0.6f, 0.8f, 1 } };
        fs[5].colorPeriod = 4;
        fs[5].flickerDepth = 0.3f;
        fs[5].flickerFrequency = 5;
        // light 6: none
        for (uint32_t i = 0; i < 6; ++i) set.set(i, fs[i]);
        set.set(7, fs[1]);
        set.clear(7);  // cleared again: directory entry 0

        std::mt19937 rng(11);
        std::uniform_real_distribution<float> u(-1, 1), time(0, 10);
        std::vector<Query> q;
        const uint32_t n = 4096;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t light = i % 8;
            float3 fw = norm({ u(rng), u(rng), u(rng) });
            float3 rt = norm(crossv(fw, norm({ u(rng), u(rng), u(rng) })));
            float3 dir = norm({ u(rng), u(rng), u(rng) });
            if (light == 3 && i % 3) dir = norm({ fw.x + 0.5f * u(rng), fw.y + 0.5f * u(rng), fw.z + 0.5f * u(rng) });  // mostly inside the cookie
            Query x{ { dir.x, dir.y, dir.z, time(rng) }, { fw.x, fw.y, fw.z, 0 }, { rt.x, rt.y, rt.z, 0 } };
            std::memcpy(&x.right.w, &light, 4);
            q.push_back(x);
        }
        auto compare = [&](const std::vector<float4>& gpu, const char* what) {
            float worst[8] = {};
            uint32_t edges = 0, inside = 0;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t l = lightOf(q[i]);
                const float3 fw{ q[i].forward.x, q[i].forward.y, q[i].forward.z }, rt{ q[i].right.x, q[i].right.y, q[i].right.z };
                const float3 dir{ q[i].dir.x, q[i].dir.y, q[i].dir.z };
                const float t = q[i].dir.w;
                const lights::LightFunction* f = set.get(l);
                const float3 cpu = f ? lights::evaluate(*f, fw, rt, dir, t) : float3{ 1, 1, 1 };
                const float3 g{ gpu[i].x, gpu[i].y, gpu[i].z };
                const float scale = std::max({ 1.0f, std::abs(cpu.x), std::abs(cpu.y), std::abs(cpu.z) });
                // images: half-float texels (2^-11 relative) and the sampler's 8-bit filter fraction; IES and time: float
                const float tol = (l == 3 || l == 4) ? 2e-3f : 1e-4f * scale;
                const float e = maxAbs(cpu, g);
                if (l == 3 && cpu.x + cpu.y + cpu.z > 0) ++inside;
                if (e <= tol)
                {
                    worst[l] = std::max(worst[l], e);
                    continue;
                }
                // a discontinuity (frame or range edge): some direction 1e-4 rad away must agree with the GPU
                bool edge = false;
                const float3 a = norm(crossv(dir, rt)), b = crossv(dir, a);
                for (int s = 0; s < 8 && !edge; ++s)
                {
                    const float ang = s * kPi / 4;
                    const float3 d2 = norm({ dir.x + 1e-4f * (std::cos(ang) * a.x + std::sin(ang) * b.x), dir.y + 1e-4f * (std::cos(ang) * a.y + std::sin(ang) * b.y),
                                             dir.z + 1e-4f * (std::cos(ang) * a.z + std::sin(ang) * b.z) });
                    edge = maxAbs(lights::evaluate(*f, fw, rt, d2, t), g) <= tol;
                }
                S_CHECK(edge, "%s: query %u light %u dir (%.5f %.5f %.5f) t %.3f: GPU (%.6f %.6f %.6f) CPU (%.6f %.6f %.6f)", what, i, l, dir.x, dir.y, dir.z, t, g.x, g.y,
                        g.z, cpu.x, cpu.y, cpu.z);
                ++edges;
            }
            S_CHECK(edges <= n / 100, "%s: %u queries at discontinuities", what, edges);
            S_CHECK(inside >= 200, "%s: only %u cookie queries inside the image", what, inside);
            std::printf("%s: GPU = CPU over %u queries - worst IES %.1e / %.1e / %.1e, cookie %.1e, gobo %.1e, time %.1e, none %.1e, cleared %.1e; %u at "
                        "discontinuities, %u inside the cookie\n",
                        what, n, worst[0], worst[1], worst[2], worst[3], worst[4], worst[5], worst[6], worst[7], edges, inside);
        };
        bool valid = false;
        const std::vector<float4> first = gpuEvaluate(tf, q, &valid);
        S_CHECK(valid, "no light function table");
        compare(first, "frame 1");
        const std::vector<float4> second = gpuEvaluate(tf, q);
        S_CHECK(std::memcmp(first.data(), second.data(), first.size() * 16) == 0, "frame 2 differs from frame 1 (nothing changed)");
        fs[3].image = smoothImage(40, 40, 2.0f);
        set.set(3, fs[3]);
        compare(gpuEvaluate(tf, q), "changed cookie");

        // ---- footprint mip level ----
        {
            lights::LightFunction stripes;
            stripes.profile = lights::Profile::Cookie;
            stripes.tanX = stripes.tanY = 1;
            stripes.image.width = stripes.image.height = 64;
            stripes.image.rgb.resize(64 * 64 * 3);
            for (uint32_t y = 0; y < 64; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                    for (int c = 0; c < 3; ++c) stripes.image.rgb[(y * 64 + x) * 3 + c] = (float)(x & 1);
            set.set(3, stripes);
            std::vector<Query> s;
            const float texel = 2.0f / 64;  // texel angle at the centre: 2 tanX / width
            for (uint32_t i = 0; i < 512; ++i)
            {
                const float3 dir = norm({ 0.5f * u(rng), 0.5f * u(rng), 1 });
                Query x{ { dir.x, dir.y, dir.z, 0 }, { 0, 0, 1, i < 256 ? 8 * texel : 0.0f }, { 1, 0, 0, 0 } };
                const uint32_t light = 3;
                std::memcpy(&x.right.w, &light, 4);
                s.push_back(x);
            }
            const std::vector<float4> r = gpuEvaluate(tf, s);
            float worst = 0, spread = 0;
            for (uint32_t i = 0; i < 256; ++i) worst = std::max(worst, std::abs(r[i].x - 0.5f));
            float lo = 1, hi = 0;
            for (uint32_t i = 256; i < 512; ++i)
            {
                lo = std::min(lo, r[i].x);
                hi = std::max(hi, r[i].x);
            }
            spread = hi - lo;
            S_CHECK(worst <= 2e-3f, "8-texel footprint: %g from the stripes' mean", worst);
            S_CHECK(spread >= 0.9f, "footprint 0: stripes not resolved (%g .. %g)", lo, hi);
            std::printf("footprint: 1-texel stripes through an 8-texel footprint -> mean 0.5 (worst %.1e); at footprint 0 sharp (%.3f .. %.3f)\n", worst, lo, hi);
        }

        for (uint32_t i = 0; i < 8; ++i) set.clear(i);
        bool stillValid = true;
        gpuEvaluate(tf, { q.begin(), q.begin() + 64 }, &stillValid);
        S_CHECK(!stillValid, "cleared functions still publish a table");
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("light function tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
