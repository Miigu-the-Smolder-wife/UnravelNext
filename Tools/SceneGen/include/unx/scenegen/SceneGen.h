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
    ForestCombat = 7, // gate: RPP-1 forest/combat section, closed canopy at eye level (eye, up, edge cameras)
    // New scenes are appended (never renumbered) through the interface-change procedure.
};

struct Request
{
    SceneId id = SceneId::CityBlock;
    uint64_t seed = 1;
    // Content scale for gate loads (1 = the scene's gate definition, e.g. >= 10 M triangles for the P1 gate scene).
    float scale = 1.0f;
};

// RPP-1 dynamic content of a section scene at tick t0 (city_block, city_night, forest_combat, waterside, interior; empty
// for the others): 1,024 rigid bodies, also present in the generated scene as InstanceDynamic instances, and 256
// character slots (not in the scene: no character assets yet). Exported as Cache/Scenes/<scene>_bodies.json (format 1,
// agreed with I; the host dynamic gate replays the motion kinematically).
struct DynamicBody
{
    std::string kind;            // crate, debris, chunk, car, log, branch, barrel, rock, smallbox
    std::string shape;           // "box" or "cylinder" (axis local +Y)
    float scale = 1;             // uniform scale of the kind's base mesh
    float3 halfExtents;          // box half extents; cylinder (radius, half height, radius)
    float3 position;             // centre at t0 (scene coordinates)
    float4 rotation{ 0, 0, 0, 1 };  // quaternion (x, y, z, w)
    std::string motion;          // "resting", "falling" (p0 + v t + g t^2 / 2 until the centre reaches restHeight), "rolling" (p0 + v t)
    float3 velocity, angularVelocity;
    float restHeight = 0;        // falling
    float period = 0;            // falling / rolling: the motion restarts every period seconds
    uint32_t instance = 0;       // index of the body's instance in the generated scene
};
struct CharacterSlot
{
    float3 position;             // feet
    float yaw = 0;
    bool hero = false;           // one of the 8 characters with hair (nearest the gate camera)
};
struct DynamicContent
{
    std::string section;
    std::vector<DynamicBody> bodies;
    std::vector<CharacterSlot> characters;
};
DynamicContent dynamicContent(const Request& request);  // instance indices are set by generate(); use generateWithContent
std::string dynamicContentJson(const Request& request, const DynamicContent& content);
// generate() and the scene's dynamic content with instance indices filled in.
scene::Scene generateWithContent(const Request& request, DynamicContent& content);

// Throws unx::Error for unknown ids. The returned scene passes scene::validate() and contains at least one camera and
// one camera path (static and moving) for gates and temporal-stability tests.
scene::Scene generate(const Request& request);

std::vector<SceneId> allScenes();
const char* sceneName(SceneId id);  // stable file-name-safe name, e.g. "city_block"
} // namespace unx::scenegen
