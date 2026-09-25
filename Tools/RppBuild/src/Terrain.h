#pragma once
// RPP-1 world terrain (Content/RPP1/rpp1_manifest.json "world.terrain"). One heightfield over the 2 x 2 km zone:
// C's rollingTerrain outside the districts, each district's own ground (city flat core, lakeFloor, lodge pad) inside,
// smoothstep rings between. The district grounds come from C's public query scenegen::terrainHeight (INTERFACES 10.1).
#include "unx/core/Math.h"
#include "unx/scene/SceneData.h"

namespace unx::rpp
{
constexpr float kPi = 3.14159265358979323846f;

// Placement of a C gate scene's local frame in the world: rotation about +Y by yaw, then translation.
struct Anchor
{
    float3 translation{};
    float yaw = 0;  // radians
    float3 toWorld(float3 p) const;
    float3 toLocal(float3 p) const;
    float3 rotate(float3 v) const;
    float3x4 matrix() const;
    float4 quaternion() const;  // (x, y, z, w) of the yaw rotation
};

struct Layout
{
    Anchor city, lake, lodge;  // forest = identity (C forest_combat frame)
    float2 standCentre{ 300.0f, -200.0f };
    float standKeepOut = 150.0f;  // no district blend weight inside this radius around the combat stand
    // City: Chebyshev distance from the city anchor; flat core, blend ring (C cityTerrain's own radii).
    float cityCore = 260.0f, cityRing = 420.0f, citySquare = 195.0f;
    // Lake: basin and bay of lakeFloor, widened by the ring.
    float lakeMargin0 = 60.0f, lakeMargin1 = 130.0f;
    // Lodge pad: flat within padRadius, blended to the lake ground by padRing.
    float padRadius = 12.0f, padRing = 20.0f;
};

namespace sg  // C's terrains through scenegen::terrainHeight (NaN outside a scene's terrain)
{
float rollingTerrain(float x, float z);
float cityTerrain(float x, float z);
float lakeFloor(float x, float z);
float smoothstep(float a, float b, float x);
} // namespace sg

class Terrain
{
public:
    explicit Terrain(const Layout& layout) : m_layout(layout) {}
    // World ground height and the weights of the districts at (x, z).
    float height(float x, float z) const;
    float cityWeight(float x, float z) const;
    float lakeWeight(float x, float z) const;
    float padWeight(float x, float z) const;
    // Ground height the source scene placed its content on, at a world position (the source's own terrain in world
    // coordinates): content moves by height(x, z) - sourceHeight(...) when it is re-seated on the world terrain.
    enum class Source { Forest, City, Lake, Lodge };
    float sourceHeight(Source s, float x, float z) const;
    // Inside the city street square (the terrain mesh leaves a hole there, the street plane covers it).
    bool inCitySquare(float x, float z) const;
    const Layout& layout() const { return m_layout; }

    // The world terrain mesh: 2 m grid over [-1000, 1000]^2, city square cut out, material 'material'.
    scene::Mesh buildMesh(uint32_t material) const;

private:
    Layout m_layout;
};

} // namespace unx::rpp
