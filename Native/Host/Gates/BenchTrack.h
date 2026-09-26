#pragma once
// Game bench camera track for the host gates (I track): the RPP-1 scene track's gamebench_<variant>.json (unx_gamebench,
// a42dff6): the scene and bodies files, one camera sample per 60 Hz tick ("camera.samples": [position[3], forward[3],
// yawDeg, pitchDeg, focalMm, fNumber, focusM, verticalFovDeg, cut]) and named cases over tick ranges ("cases":
// [{"name", "ticks": [begin, end)}]). The gate renders one frame per tick; a sample with cut = 1 starts its frame with a
// camera cut (history discontinuity). Focal length, aperture and focus are read and not used (the renderer has no
// lens yet): results say so.
#include "Json.h"

#include "unx/core/File.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace unx::host::gate
{
struct BenchSample
{
    float position[3] = {}, forward[3] = { 0, 0, -1 };
    float verticalFovDeg = 60;
    bool cut = false;
};

struct BenchCase
{
    std::string name;
    uint64_t begin = 0, end = 0;  // ticks [begin, end)
};

struct BenchTrack
{
    std::string name;
    std::filesystem::path scene, bodies;
    std::vector<BenchSample> samples;
    std::vector<BenchCase> cases;
    // The case of a tick (index into cases), or -1.
    int caseOf(uint64_t tick) const
    {
        for (size_t i = 0; i < cases.size(); ++i)
            if (tick >= cases[i].begin && tick < cases[i].end) return (int)i;
        return -1;
    }
};

inline BenchTrack loadBench(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) fail("bench file %s does not exist (unx_gamebench --out <dir>)", path.string().c_str());
    const json::Value root = json::parse(readTextFile(path));
    if (root.at("format").num() != 1) fail("%s: format %g, expected 1", path.string().c_str(), root.at("format").num());
    BenchTrack b;
    b.name = root.at("name").str();
    b.scene = path.parent_path() / root.at("scene").str();
    b.bodies = path.parent_path() / root.at("bodiesFile").str();
    const json::Value& camera = root.at("camera");
    if (camera.at("tickHz").num() != 60) fail("%s: camera at %g Hz (the gate renders one frame per 60 Hz tick)", path.string().c_str(), camera.at("tickHz").num());
    for (const json::Value& s : camera.at("samples").arr())
    {
        const std::vector<json::Value>& a = s.arr();
        if (a.size() != 9) fail("%s: a camera sample has %zu fields, expected 9", path.string().c_str(), a.size());
        BenchSample x;
        a[0].numbers(x.position);
        a[1].numbers(x.forward);
        x.verticalFovDeg = (float)a[7].num();
        x.cut = a[8].num() != 0;
        const float n = std::sqrt(x.forward[0] * x.forward[0] + x.forward[1] * x.forward[1] + x.forward[2] * x.forward[2]);
        if (!(n > 0.5f && n < 1.5f)) fail("%s: camera forward of length %g", path.string().c_str(), n);
        for (float& f : x.forward) f /= n;
        b.samples.push_back(x);
    }
    for (const json::Value& c : root.at("cases").arr())
    {
        BenchCase k;
        k.name = c.at("name").str();
        const std::vector<json::Value>& t = c.at("ticks").arr();
        if (t.size() != 2) fail("%s: case '%s' ticks are not [begin, end)", path.string().c_str(), k.name.c_str());
        k.begin = (uint64_t)t[0].num();
        k.end = (uint64_t)t[1].num();
        if (k.end <= k.begin || k.end > b.samples.size()) fail("%s: case '%s' ticks [%llu, %llu) outside the %zu samples", path.string().c_str(), k.name.c_str(),
                                                              (unsigned long long)k.begin, (unsigned long long)k.end, b.samples.size());
        b.cases.push_back(k);
    }
    if (b.samples.empty()) fail("%s: no camera samples", path.string().c_str());
    return b;
}

// Camera up without roll: world +Y made perpendicular to forward (a straight-up or -down forward keeps +Z as up).
inline void benchUp(const float forward[3], float up[3])
{
    float r[3] = { forward[1] * 0 - forward[2] * 1, forward[2] * 0 - forward[0] * 0, forward[0] * 1 - forward[1] * 0 };  // forward x (0, 1, 0)
    float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (n < 1e-6f)
    {
        up[0] = 0; up[1] = 0; up[2] = 1;
        return;
    }
    for (float& v : r) v /= n;
    up[0] = r[1] * forward[2] - r[2] * forward[1];  // right x forward
    up[1] = r[2] * forward[0] - r[0] * forward[2];
    up[2] = r[0] * forward[1] - r[1] * forward[0];
}
} // namespace unx::host::gate
