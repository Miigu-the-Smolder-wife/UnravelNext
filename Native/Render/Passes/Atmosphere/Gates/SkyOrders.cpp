// S sky model's single scattering for comparison with unx_reference --volume-order 1:1 (CPU only, no GPU lock).
//   unx_gate_atmosphere_skyorders --scene NAME --camera NAME --res WxH [--step N] [--rows A:B] [--out FILE.csv]
//                                 [--black-ground] [--write-scene FILE.unxscene]
// For a grid of pixels (every N-th, pixel centres, the unx_reference camera mapping) whose view ray leaves the atmosphere
// without meeting the planet, the double-precision S model (AtmosphereReference.h) gives the single scattering in
// exposed units (x E_TOA x sun colour x 1 / (1.2 2^ev100)) like the reference images. Scene geometry (terrain, towers)
// is not in the model: pick sky pixels. unx_reference's order window counts atmosphere events only, so its order 1 also
// holds sunlight reflected by the ground and then scattered once (in S: part of J_ms). --black-ground sets the planet
// albedo and every material to black (no diffuse, no specular) in the model and in the scene written by --write-scene,
// so that order 1 of a reference of that scene is single scattering alone. The multiple scattering (the J_ms table) is
// compared through RendererGate --capture against the full reference and its order windows (S_STATUS_KO.md 8).
// 2026-09-25 [measured, ridge_sunset 640x360]: single scattering 0.999 of the reference; Hillaire 2020 Psi_ms (the
// previous model) gave 0.80-0.94 of the multiple scattering, which J_ms replaced.
#include "AtmosphereReference.h"

#if __has_include("unx/scenegen/SceneGen.h") && defined(UNX_HAS_SCENEGEN)
#include "unx/scenegen/SceneGen.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace unx;
namespace ref = unx::render::atmosphere::reference;

namespace
{
ref::D3 d3(float3 v) { return { v.x, v.y, v.z }; }

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName, cameraName, out, writeScene;
        bool blackGround = false;
        uint32_t w = 0, h = 0, step = 16, row0 = 0, row1 = 0xFFFFFFFFu;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("%s needs a value", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--camera") cameraName = next();
            else if (a == "--res")
            {
                const std::string r = next();
                const size_t x = r.find('x');
                if (x == std::string::npos) fail("--res expects WxH");
                w = (uint32_t)std::stoul(r.substr(0, x));
                h = (uint32_t)std::stoul(r.substr(x + 1));
            }
            else if (a == "--step") step = (uint32_t)std::stoul(next());
            else if (a == "--rows")
            {
                const std::string r = next();
                const size_t c = r.find(':');
                if (c == std::string::npos) fail("--rows expects A:B");
                row0 = (uint32_t)std::stoul(r.substr(0, c));
                row1 = (uint32_t)std::stoul(r.substr(c + 1));
            }
            else if (a == "--out") out = next();
            else if (a == "--black-ground") blackGround = true;
            else if (a == "--write-scene") writeScene = next();
            else fail("unknown argument %s", a.c_str());
        }
        if (sceneName.empty() || cameraName.empty() || w == 0 || h == 0) fail("--scene, --camera and --res are required");
        scenegen::Request request;
        bool found = false;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        scene::Scene s = scenegen::generate(request);
        if (blackGround)
        {
            s.atmosphere.groundAlbedo = { 0, 0, 0 };
            for (scene::Material& mat : s.materials)
            {
                mat.baseColor = { 0, 0, 0 };
                mat.specular = 0;
                mat.metallic = 0;
            }
        }
        if (!writeScene.empty())
        {
            scene::save(s, writeScene);
            logf("sky orders: scene written to %s\n", writeScene.c_str());
        }
        const scene::Camera* cam = nullptr;
        for (const scene::Camera& c : s.cameras)
            if (c.name == cameraName) cam = &c;
        if (!cam) fail("unknown camera %s", cameraName.c_str());

        const ref::Model m = ref::fromScene(s.atmosphere);
        const ref::D3 sun = ref::normalize(d3(s.sun.direction));
        const double exposure = 1.0 / (1.2 * std::exp2((double)cam->ev100));
        const ref::D3 scale = d3(s.sun.color) * (s.sun.illuminance * exposure);

        // The unx_reference camera mapping (PathTracer.cpp cameraRay) at pixel centres.
        const ref::D3 f = ref::normalize(d3(cam->forward));
        const ref::D3 upHint = d3(cam->up);
        const ref::D3 right = ref::normalize({ f.y * upHint.z - f.z * upHint.y, f.z * upHint.x - f.x * upHint.z, f.x * upHint.y - f.y * upHint.x });
        const ref::D3 up{ right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x };
        const double th = std::tan(0.5 * cam->verticalFov), aspect = double(w) / h;
        const ref::D3 eye = d3(cam->position);

        struct Row
        {
            uint32_t x, y;
            ref::D3 single;
            bool sky;
        };
        std::vector<Row> rows;
        for (uint32_t y = step / 2; y < h; y += step)
            if (y >= row0 && y <= row1)
                for (uint32_t x = step / 2; x < w; x += step) rows.push_back({ x, y, {}, false });
        std::vector<std::thread> threads;
        const unsigned n = std::max(1u, std::thread::hardware_concurrency() / 2);
        for (unsigned t = 0; t < n; ++t)
            threads.emplace_back([&, t] {
                for (size_t i = t; i < rows.size(); i += n)
                {
                    Row& r = rows[i];
                    const double nx = (r.x + 0.5) / w * 2 - 1, ny = 1 - (r.y + 0.5) / h * 2;
                    const ref::D3 d = ref::normalize(f + right * (nx * th * aspect) + up * (ny * th));
                    r.sky = !ref::hitsGround(m, eye, d);
                    if (!r.sky) continue;
                    r.single = ref::singleScattering(m, eye, d, sun) * scale;
                }
            });
        for (auto& th2 : threads) th2.join();

        FILE* file = stdout;
        if (!out.empty() && fopen_s(&file, out.c_str(), "w") != 0) file = nullptr;
        if (!file) fail("cannot write %s", out.c_str());
        std::fprintf(file, "x,y,single_r,single_g,single_b\n");
        for (const Row& r : rows)
            if (r.sky)
                std::fprintf(file, "%u,%u,%.9g,%.9g,%.9g\n", r.x, r.y, r.single.x, r.single.y, r.single.z);
        if (file != stdout) std::fclose(file);
        logf("sky orders: %zu pixels (%s)\n", rows.size(), out.empty() ? "stdout" : out.c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
#else
#include <cstdio>
int main()
{
    std::printf("unx_gate_atmosphere_skyorders: needs scenegen (UNX_HAS_SCENEGEN)\n");
    return 0;
}
#endif
