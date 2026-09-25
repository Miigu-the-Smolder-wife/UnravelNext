#pragma once
// RPP-1 world composition (Content/RPP1/rpp1_manifest.json "world", RPP1_MANIFEST_KO.md 2-4). Generates C's four gate
// scenes (forest_combat, city_night, waterside, interior; seed 1, scale 1), drops their grounds, places the city, lake and
// lodge at their anchors on one world heightfield, keeps the vegetation totals exact (100,000 / 1,000,000), keeps every
// section's 1,024 rigid bodies (as InstanceDynamic instances; only the active section's set is visible at a time, the
// path's swap events switch them) and the section character slots, and adds the lights the gate scenes do not have.
#include "Terrain.h"

#include "unx/scenegen/SceneGen.h"

#include <array>
#include <string>
#include <vector>

namespace unx::rpp
{
enum Section : uint32_t { SectionCity = 0, SectionForest = 1, SectionWaterside = 2, SectionInterior = 3, SectionCount = 4 };
const char* sectionId(Section s);  // "city", "forest", "waterside", "interior"

struct SectionContent
{
    scenegen::SceneId source;
    Anchor anchor;
    scenegen::DynamicContent content;  // world coordinates; body.instance = world instance index
};

struct WorldStats
{
    uint32_t trees = 0, treesStand = 0, treesCity = 0, treesLake = 0, treesBase = 0, treesFill = 0;
    uint32_t grass = 0, grassReeds = 0, grassBase = 0, grassFill = 0;
    uint32_t lights = 0, lightsShadowed = 0;
    uint32_t lightsBySection[SectionCount]{}, shadowedBySection[SectionCount]{};
    uint32_t dynamicBodies = 0, staticInstances = 0;
    uint64_t triangles = 0;  // mesh triangles x instances (as submitted, before culling)
};

// Points the path needs from the composition (world coordinates).
struct PathAnchors
{
    std::vector<float3> road;        // forest road centreline (city east exit -> stand edge), tree-free corridor
    float3 lodgeDoorOutside{}, lodgeDoorInside{};
    int lodgeDoorWall = +1;          // +1: door in the +X wall, -1: in the -X wall (lodge-local)
    uint32_t neonFirst = 0, neonCount = 0;      // city neon tubes (light animation)
    uint32_t muzzleFirst = 0, muzzleCount = 0;  // forest muzzle-flash lights (scene intensity 0 = off; the path turns them on)
    float muzzleIntensity = 20000.0f;
};

struct World
{
    scene::Scene scene;
    Layout layout;
    PathAnchors anchorsForPath;
    std::array<SectionContent, SectionCount> sections;
    WorldStats stats;
    std::array<std::string, SectionCount> sourceHashes;  // contentHash of C's generated scenes (identity of the input)
};

World buildWorld(const Layout& layout, uint64_t seed);

// C bodies file format 1 (BodiesFile.h reads it) for one section of the world: world positions, world instance indices.
std::string sectionBodiesJson(const World& world, Section s);
} // namespace unx::rpp
