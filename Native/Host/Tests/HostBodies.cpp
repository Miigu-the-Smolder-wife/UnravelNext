// RPP-1 body placement file and its kinematic replay (I track, Gates/BodiesFile.h), CPU only: a falling body stops with
// its centre at restHeight, a rolling body moves at its velocity and spins at its angular velocity, every period starts
// over and is reported as a restart (teleport), the scene's t0 scale survives the motion, and malformed files fail.
#include "../Gates/BodiesFile.h"

#include "unx/core/File.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace unx;
using namespace unx::host;

namespace
{
const char* kFile = R"({
  "format": 1, "scene": "test", "seed": 1, "scale": 1, "section": "test", "gravity": [0, -9.81, 0], "t0": 0,
  "mix": { "resting": 1, "falling": 1, "rolling": 1 },
  "bodies": [
    { "instance": 3, "kind": "crate", "shape": "box", "halfExtents": [0.5, 0.5, 0.5], "position": [1, 0.5, 2], "rotation": [0, 0, 0, 1],
      "motion": "resting", "velocity": [0, 0, 0], "angularVelocity": [0, 0, 0], "restHeight": 0, "period": 0 },
    { "instance": 4, "kind": "debris", "shape": "box", "halfExtents": [0.25, 0.25, 0.25], "position": [0, 10.25, 0], "rotation": [0, 0, 0, 1],
      "motion": "falling", "velocity": [0, 0, 0], "angularVelocity": [0, 0, 0], "restHeight": 0.25, "period": 3 },
    { "instance": 5, "kind": "barrel", "shape": "cylinder", "halfExtents": [0.3, 0.45, 0.3], "position": [0, 0.3, 0], "rotation": [0, 0, 0, 1],
      "motion": "rolling", "velocity": [2, 0, 0], "angularVelocity": [0, 0, -6.6666667], "restHeight": 0, "period": 5 }
  ],
  "characters": [ { "position": [4, 0, 4], "yaw": 1.5, "hair": true } ]
})";
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-66s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "unx_test_host_bodies";
        std::filesystem::create_directories(dir);
        const std::filesystem::path path = dir / "test_bodies.json";
        writeTextFile(path, kFile);
        gate::BodiesFile f = gate::loadBodies(path);
        expect("three bodies, one character, mix 1/1/1", f.bodies.size() == 3 && f.characters.size() == 1 && f.resting == 1 && f.falling == 1 && f.rolling == 1);
        expect("instances and kinds read", f.bodies[1].instance == 4 && f.bodies[2].kind == "barrel");
        // Scene t0 transform of the rolling body with a uniform scale 2 (the replay must keep it).
        const float base[12] = { 2, 0, 0, 0, 0, 2, 0, 0.3f, 0, 0, 2, 0 };
        std::memcpy(f.bodies[2].base, base, sizeof base);
        float m[12];

        gate::bodyAt(f.bodies[0], f.gravity, 7.0, 6.9, m);
        expect("resting body stays at its position", m[3] == 1 && m[7] == 0.5f && m[11] == 2);

        // Falling 10 m from rest: hits after sqrt(2 * 10 / 9.81) = 1.428 s.
        gate::bodyAt(f.bodies[1], f.gravity, 1.0, 0.99, m);
        expect("falling body after 1 s is at 10.25 - 4.905", std::fabs(m[7] - (10.25f - 4.905f)) < 1e-4f);
        gate::bodyAt(f.bodies[1], f.gravity, 2.5, 2.49, m);
        expect("falling body after landing rests with its centre at restHeight", std::fabs(m[7] - 0.25f) < 1e-4f);
        const bool restart = gate::bodyAt(f.bodies[1], f.gravity, 3.01, 2.99, m);
        expect("a new period starts over (restart reported, back near the top)", restart && m[7] > 10.2f);
        expect("no restart inside a period", !gate::bodyAt(f.bodies[1], f.gravity, 2.0, 1.99, m));

        // Rolling: x = 2 t, spin -6.667 rad/s about z (radius 0.3: rolling without slip at 2 m/s).
        gate::bodyAt(f.bodies[2], f.gravity, 0.75, 0.74, m);
        const float angle = -6.6666667f * 0.75f, c = std::cos(angle), s = std::sin(angle);
        expect("rolling body moved v t", std::fabs(m[3] - 1.5f) < 1e-5f && std::fabs(m[7] - 0.3f) < 1e-6f);
        expect("rolling body turned w t about z, scale 2 kept",
               std::fabs(m[0] - 2 * c) < 1e-4f && std::fabs(m[1] + 2 * s) < 1e-4f && std::fabs(m[4] - 2 * s) < 1e-4f && std::fabs(m[5] - 2 * c) < 1e-4f && std::fabs(m[10] - 2) < 1e-5f);

        // Malformed files fail: wrong mix, unknown motion.
        auto fails = [&](std::string text) {
            writeTextFile(path, text);
            try
            {
                gate::loadBodies(path);
                return false;
            }
            catch (const std::exception&)
            {
                return true;
            }
        };
        std::string wrongMix = kFile;
        wrongMix.replace(wrongMix.find("\"resting\": 1"), 12, "\"resting\": 2");
        expect("a mix that disagrees with the bodies fails", fails(wrongMix));
        std::string badMotion = kFile;
        badMotion.replace(badMotion.find("\"motion\": \"rolling\""), 19, "\"motion\": \"sliding\"");
        expect("an unknown motion fails", fails(badMotion));
        expect("a missing file fails", [&] {
            try { gate::loadBodies(dir / "missing.json"); return false; }
            catch (const std::exception&) { return true; }
        }());
        std::filesystem::remove_all(dir);
        logf(failures ? "HOST BODIES TEST FAILED (%u)\n" : "HOST BODIES TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
