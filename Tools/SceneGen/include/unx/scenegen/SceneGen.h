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
    FurnaceRoom = 8,  // diagnostic (diagnosticScenes): a closed Lambert room with one point light - the light every stage
                      // of the indirect lighting must hold is known in closed form (kFurnace*, Tools/Verify/furnace.py)
    FurnaceRoomDay = 9,  // the same room with the sun up outside: the same numbers hold, and whatever a stage lets
                         // through the walls reads as light over them (a leak is thousands of times the room's light)
    ShadingBall = 10,    // diagnostic: the shading models side by side - five spheres (Standard, Subsurface, Subsurface
                         // with one lobe, sheen, clearcoat) and a thin Subsurface slab with a light behind it; above
                         // them a sphere with the cloth blend and two eyes (the eye model) that look at the front camera
    HairBall = 11,       // diagnostic: strand hair in two colours - two head-sized spheres under a dark and a blond groom
                         // (grooms), a key light, a rim light behind them, a dim sun
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

// Strand grooms of a scene (hair_ball; empty for the others). The scene file holds no strands: whoever renders the
// scene makes a hair body of each groom (unx/hair/Hair.h BodyDesc: one joint at 'head', the rest positions relative
// to it, the head's sphere as the collision capsule), as the gates do for their characters' hair.
struct Groom
{
    float3 head;                        // the joint: the head sphere's centre (scene coordinates)
    float headRadius = 0;               // the sphere under the hair (m)
    uint32_t material = 0;              // scene material (Hair class)
    uint32_t nodesPerStrand = 0;
    std::vector<float3> restPositions;  // guides x nodesPerStrand, relative to 'head'
    struct Follow
    {
        uint32_t guide;
        float3 offset;                  // at the root, in the guide's rest frame (x along the root segment)
        float tipSpread;                // offset scale at the tip
    };
    std::vector<Follow> follows;
    float rootRadius = 40e-6f, tipRadius = 20e-6f;  // metres
};
std::vector<Groom> grooms(const Request& request);

// Ground height of a scene's terrain at scene-local (x, z) (RPP-1 world assembly): the analytic function its terrain
// mesh samples (the mesh vertices lie on it exactly; between them the mesh is linear over its grid: city 2 m, forest
// 1.95 m, waterside 2 m, ridge 20 m). City scenes: 0 over the city square, which the street plane covers. Interior:
// the outside ground at -0.2 m. NaN outside the terrain's extent. Independent of the seed and scale.
float terrainHeight(SceneId id, float x, float z);

// Throws unx::Error for unknown ids. The returned scene passes scene::validate() and contains at least one camera and
// one camera path (static and moving) for gates and temporal-stability tests.
scene::Scene generate(const Request& request);

std::vector<SceneId> allScenes();
// Scenes that measure the renderer instead of showing something (not in allScenes: the gates' scene sweeps and the
// scene tests keep their set). generate() and sceneName() know them.
std::vector<SceneId> diagnosticScenes();
// FurnaceRoom: the room's inner size (m, centred on x and z, floor at y = 0), its walls' albedo and the light's
// intensity (cd, at the room's centre). A closed room of uniform Lambert albedo rho holding a flux Phi keeps, in the
// steady state, a mean irradiance Phi / (A (1 - rho)) over its inner surface A: the direct light's mean is Phi / A, the
// first bounce's rho Phi / A, all bounces' rho / (1 - rho) x Phi / A. With these numbers: Phi = 4 pi x 100 = 1256.6 lm,
// A = 256 m2, direct 4.909 lux, first bounce 2.454 lux, all bounces 4.909 lux.
constexpr float kFurnaceWidth = 8.0f, kFurnaceHeight = 4.0f, kFurnaceDepth = 8.0f, kFurnaceAlbedo = 0.5f, kFurnaceCandela = 100.0f;
const char* sceneName(SceneId id);  // stable file-name-safe name, e.g. "city_block"
} // namespace unx::scenegen
