#pragma once
// Procedural test scenes (INTERFACES_KO.md 10.1). Owner: C. Signatures fixed by the interface; the scenes' content
// is C's. Every scene is generated in code (no authored assets), deterministically: the same request yields the same
// bytes (scene::contentHash), so reference images can be cached by hash.
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <string>
#include <vector>

namespace unx::scenegen
{
enum class SceneId : uint32_t
{
    CityBlock = 0,    // P0b/P1: buildings, streets, terrain, sun + sky (static)
    ForestThin = 1,   // P1/P3: 6 cm leaves, 4 mm grass blades (coverage layer, bricks), wind
    ForestCard = 2,   // P1/P3: 35 cm card leaves, 30 cm grass cards
    Waterside = 3,    // P2/P4: calm water (planar mirror) + wave region, wet rocks
    Interior = 4,     // P2: mirror, glossy floor, area lights
    CityNight = 5,    // P2: 512 local lights (128 shadowed), wet road
    RidgeSunset = 6,  // S: distant ridge and tower shadows in the air (god rays) against a low sun, 20 x 20 km
    // New scenes are appended (never renumbered) through the interface-change procedure.
};

struct Request
{
    SceneId id = SceneId::CityBlock;
    uint64_t seed = 1;
    // Content scale for gate loads (1 = the scene's gate definition, e.g. >= 10 M triangles for the P1 gate scene).
    float scale = 1.0f;
};

// Throws unx::Error for unknown ids. The returned scene passes scene::validate() and contains at least one camera and
// one camera path (static and moving) for gates and temporal-stability tests.
scene::Scene generate(const Request& request);

std::vector<SceneId> allScenes();
const char* sceneName(SceneId id);  // stable file-name-safe name, e.g. "city_block"
} // namespace unx::scenegen
