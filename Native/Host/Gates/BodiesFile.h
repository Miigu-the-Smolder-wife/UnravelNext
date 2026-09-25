#pragma once
// RPP-1 rigid-body placements for the host gates (I track): C's generator writes <scene>_bodies.json next to the scene it
// generates (unx_scenegen --scene <s> --out <dir> --bodies <dir>/<s>_bodies.json; format 1, C 195fda3): per body its
// scene instance (dynamic, starting at 'position'), kind, shape, transform and motion class; per character slot the feet
// position and yaw. The gate replays each body's motion kinematically, so every run moves the same bodies the same way:
//   resting  fixed
//   falling  p = p0 + v t + g t^2 / 2 until the centre reaches restHeight, then at rest there
//   rolling  p = p0 + v t, orientation exp(w t / 2) q0
// Every 'period' seconds a body starts over from its initial state (that frame is a teleport: no motion), so the share of
// moving bodies stays the scene's for any run length. A missing file or field is an error (no fallback placement).
#include "Json.h"

#include "unx/core/File.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace unx::host::gate
{
struct Body
{
    enum class Motion { Resting, Falling, Rolling } motion = Motion::Resting;
    uint32_t instance = 0;                                 // the scene's instance of this body
    std::string kind;                                      // crate, debris, car, log, ... (report only)
    float halfExtents[3] = {};                             // box: half sizes; cylinder: radius, half height, radius (axis +Y)
    float position[3] = {}, rotation[4] = { 0, 0, 0, 1 };  // quaternion x, y, z, w
    float velocity[3] = {}, angularVelocity[3] = {};
    float restHeight = 0, period = 0;
    // The scene instance's object-to-world at t0 (row-major 3 x 4; set by the gate from the scene): its 3 x 3 carries the
    // body's shape scale, which the motion keeps (the replay rotates and moves it, never rebuilds it).
    float base[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
};

struct CharacterSlot
{
    float position[3] = {};  // feet
    float yaw = 0;           // radians about +Y
};

struct BodiesFile
{
    std::string scene, section;
    uint64_t seed = 0;
    float gravity[3] = {};
    uint32_t resting = 0, falling = 0, rolling = 0;  // "mix"
    std::vector<Body> bodies;
    std::vector<CharacterSlot> characters;
};

inline BodiesFile loadBodies(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) fail("bodies file %s does not exist (C's RPP-1 placement generator writes it)", path.string().c_str());
    const json::Value root = json::parse(readTextFile(path));
    if (root.at("format").num() != 1) fail("%s: format %g, expected 1", path.string().c_str(), root.at("format").num());
    BodiesFile f;
    f.scene = root.at("scene").str();
    f.section = root.at("section").str();
    f.seed = (uint64_t)root.at("seed").num();
    root.at("gravity").numbers(f.gravity);
    if (root.at("t0").num() != 0) fail("%s: t0 %g (the replay starts at 0)", path.string().c_str(), root.at("t0").num());
    const json::Value& mix = root.at("mix");
    f.resting = (uint32_t)mix.at("resting").num();
    f.falling = (uint32_t)mix.at("falling").num();
    f.rolling = (uint32_t)mix.at("rolling").num();
    for (const json::Value& b : root.at("bodies").arr())
    {
        Body body;
        body.instance = (uint32_t)b.at("instance").num();
        body.kind = b.at("kind").str();
        const std::string& shape = b.at("shape").str();
        if (shape != "box" && shape != "cylinder") fail("%s: body shape '%s'", path.string().c_str(), shape.c_str());
        b.at("halfExtents").numbers(body.halfExtents);
        b.at("position").numbers(body.position);
        b.at("rotation").numbers(body.rotation);
        const std::string& m = b.at("motion").str();
        if (m == "resting") body.motion = Body::Motion::Resting;
        else if (m == "falling") body.motion = Body::Motion::Falling;
        else if (m == "rolling") body.motion = Body::Motion::Rolling;
        else fail("%s: motion '%s'", path.string().c_str(), m.c_str());
        if (body.motion != Body::Motion::Resting)
        {
            b.at("velocity").numbers(body.velocity);
            body.period = (float)b.at("period").num();
            if (!(body.period > 0)) fail("%s: a moving body needs a period > 0", path.string().c_str());
        }
        if (body.motion == Body::Motion::Falling) body.restHeight = (float)b.at("restHeight").num();
        if (body.motion == Body::Motion::Rolling) b.at("angularVelocity").numbers(body.angularVelocity);
        const float q = std::sqrt(body.rotation[0] * body.rotation[0] + body.rotation[1] * body.rotation[1] + body.rotation[2] * body.rotation[2] + body.rotation[3] * body.rotation[3]);
        if (std::fabs(q - 1) > 1e-3f) fail("%s: rotation is not a unit quaternion (|q| = %g)", path.string().c_str(), q);
        f.bodies.push_back(body);
    }
    if (f.bodies.empty()) fail("%s: no bodies", path.string().c_str());
    uint32_t counted[3] = {};
    for (const Body& b : f.bodies) ++counted[(int)b.motion];
    if (counted[0] != f.resting || counted[1] != f.falling || counted[2] != f.rolling)
        fail("%s: mix says %u/%u/%u, bodies are %u/%u/%u", path.string().c_str(), f.resting, f.falling, f.rolling, counted[0], counted[1], counted[2]);
    for (const json::Value& c : root.at("characters").arr())
    {
        CharacterSlot slot;
        c.at("position").numbers(slot.position);
        slot.yaw = (float)c.at("yaw").num();
        f.characters.push_back(slot);
    }
    return f;
}

// The body at time t (s): row-major 3 x 4 object-to-world (the ABI's UnxTransformUpdate::transform) = the motion's
// rotation since t0 times the scene's t0 3 x 3 (base), at the motion's position. Returns true when this time falls in a
// new period relative to 'previous' (the host marks the update as a teleport).
inline bool bodyAt(const Body& b, const float gravity[3], double t, double previous, float out[12])
{
    double local = t;
    bool restarted = false;
    if (b.motion != Body::Motion::Resting)
    {
        local = std::fmod(t, (double)b.period);
        restarted = previous >= 0 && std::floor(t / b.period) != std::floor(previous / b.period);
    }
    float p[3] = { b.position[0], b.position[1], b.position[2] };
    float q[4] = { 0, 0, 0, 1 };  // rotation since t0
    const float s = (float)local;
    if (b.motion == Body::Motion::Falling)
    {
        // Time at which the centre reaches restHeight (the later root of y0 + vy t + gy t^2 / 2 = restHeight).
        float stop = s;
        const float a = 0.5f * gravity[1], bb = b.velocity[1], c = b.position[1] - b.restHeight;
        if (std::fabs(a) > 0)
        {
            const float disc = bb * bb - 4 * a * c;
            if (disc >= 0)
            {
                const float r0 = (-bb - std::sqrt(disc)) / (2 * a), r1 = (-bb + std::sqrt(disc)) / (2 * a);
                const float hit = std::fmax(r0, r1);
                if (hit >= 0) stop = std::fmin(s, hit);
            }
        }
        for (int i = 0; i < 3; ++i) p[i] += b.velocity[i] * stop + 0.5f * gravity[i] * stop * stop;
    }
    else if (b.motion == Body::Motion::Rolling)
    {
        for (int i = 0; i < 3; ++i) p[i] += b.velocity[i] * s;
        const float w = std::sqrt(b.angularVelocity[0] * b.angularVelocity[0] + b.angularVelocity[1] * b.angularVelocity[1] + b.angularVelocity[2] * b.angularVelocity[2]);
        if (w > 0)
        {
            const float half = 0.5f * w * s, k = std::sin(half) / w;
            q[0] = b.angularVelocity[0] * k;
            q[1] = b.angularVelocity[1] * k;
            q[2] = b.angularVelocity[2] * k;
            q[3] = std::cos(half);
        }
    }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
            out[row * 4 + col] = r[row * 3 + 0] * b.base[0 * 4 + col] + r[row * 3 + 1] * b.base[1 * 4 + col] + r[row * 3 + 2] * b.base[2 * 4 + col];
        out[row * 4 + 3] = p[row];
    }
    return restarted;
}
} // namespace unx::host::gate
