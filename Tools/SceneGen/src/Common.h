#pragma once
// Shared building blocks of the procedural test scenes (C track). Everything here is deterministic: integer RNG only
// (no std:: distributions, whose output is implementation-defined), sequential generation, fixed iteration orders.
#include "unx/scene/SceneData.h"
#include "unx/scenegen/SceneGen.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace unx::scenegen::detail
{
using scene::Instance;
using scene::Material;
using scene::Mesh;
using scene::Scene;
using scene::Texture;

constexpr float kPi = 3.14159265358979323846f;

// PCG32 (O'Neill 2014), one stream per purpose so adding content to one part of a scene does not reshuffle another.
struct Rng
{
    uint64_t state = 0, inc = 1;
    Rng(uint64_t seed, uint64_t stream);
    uint32_t next();
    float uniform() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
    uint32_t below(uint32_t n) { return (uint32_t)(((uint64_t)next() * n) >> 32); }
};

inline float3 f3(float x, float y, float z) { return { x, y, z }; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
inline float smoothstepf(float a, float b, float x)
{
    const float t = clampf((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

// Object-to-world: rotation about +Y (yaw), then an optional tilt about the local X axis, uniform scale, translation.
float3x4 placement(float3 position, float yaw, float scale = 1.0f, float tilt = 0.0f);
// Rotation that maps +Y to 'up' and +X towards 'right' (orthonormalised), with uniform scale.
float3x4 frame(float3 position, float3 up, float3 right, float scale = 1.0f);

// Value noise and fBm on a periodic lattice (period in lattice cells), deterministic from a seed.
float valueNoise2(float x, float y, uint32_t seed, int period = 0);
float fbm2(float x, float y, uint32_t seed, int octaves, int period = 0);

// Triangle-mesh builder. Positions/normals/uv are appended; tangents are computed once at the end from uv0 when the
// mesh needs them (normal-mapped materials), so the reference and the renderer read the same tangent frame.
struct MeshBuilder
{
    Mesh mesh;
    uint32_t currentMaterial = 0;
    uint32_t submeshStart = 0;

    explicit MeshBuilder(std::string name) { mesh.name = std::move(name); }
    uint32_t vertex(float3 p, float3 n, float2 uv);
    void triangle(uint32_t a, uint32_t b, uint32_t c) { mesh.indices.insert(mesh.indices.end(), { a, b, c }); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { triangle(a, b, c); triangle(a, c, d); }
    // Starts a new submesh with this material (closes the previous one if it has triangles).
    void material(uint32_t m);
    Mesh finish(bool withTangents);

    // Primitives (counter-clockwise front faces seen from outside). UVs in metres * uvScale for tiling materials.
    void box(float3 lo, float3 hi, float uvScale = 1.0f);
    void cylinder(float3 base, float3 axis, float r0, float r1, int segments, int rings, bool caps, float uvScale = 1.0f);
    void sphere(float3 centre, float radius, int segments, int rings, float uvScale = 1.0f);
    void quadXZ(float x0, float z0, float x1, float z1, float y, float uvScale = 1.0f);  // facing +Y
    // Planar polygon quad given 4 corners (counter-clockwise seen from the normal side) with explicit UVs.
    void quad4(float3 p0, float3 p1, float3 p2, float3 p3, float2 t0, float2 t1, float2 t2, float2 t3);
    // Heightfield grid over [x0,x1] x [z0,z1] with n x n quads; normals from central differences of 'height'.
    void heightfield(float x0, float z0, float x1, float z1, int n, const std::function<float(float, float)>& height, float uvScale);
};

void computeTangents(Mesh& mesh);

// Textures (mip 0 only; consumers build their own chains).
Texture makeTexture(std::string name, uint32_t w, uint32_t h, scene::TextureFormat format, bool wrap = true);
uint8_t toSrgb8(float linear);
uint8_t toUnorm8(float v);
// Normal map (Rg8Normal) from a periodic height function h(u,v) in metres over a tile of 'tileMetres'.
Texture normalMapFromHeight(std::string name, uint32_t size, float tileMetres, const std::function<float(float, float)>& height);

uint32_t addTexture(Scene& s, Texture t);
uint32_t addMaterial(Scene& s, Material m);
uint32_t addMesh(Scene& s, Mesh m);
Instance& addInstance(Scene& s, uint32_t mesh, const float3x4& transform, uint32_t flags = scene::InstanceCastShadow);

// Shared material set (textures generated once per scene).
struct Palette
{
    uint32_t asphalt = 0, asphaltWet = 0, concrete = 0, brick = 0, plaster = 0, windowGlass = 0, windowLit = 0, metal = 0,
             roofing = 0, grass = 0, soil = 0, bark = 0, rock = 0, rockWet = 0, sand = 0;
};
Palette buildPalette(Scene& s, uint64_t seed, bool wetRoads);

// Terrain height functions (metres).
float rollingTerrain(float x, float z);          // the microbench terrain (6 m + 2 m + 0.7 m waves)
float cityTerrain(float x, float z);             // gentle, near 0 inside the city area
float lakeFloor(float x, float z);               // waterside: lake basin and wave bay (water plane at 0)

// Sun direction (unit, towards the sun) from elevation above the horizon and azimuth measured from +X towards +Z.
float3 sunDirection(float elevationDegrees, float azimuthDegrees);
// Colour scaled to Rec.709 luminance 1 (Light::color convention).
float3 luminanceNormalised(float3 c);

// Common cameras/paths.
scene::Camera camera(std::string name, float3 position, float3 target, float ev100, float verticalFovDegrees = 60.0f);
scene::CameraPath staticPath(const scene::Camera& c);
// Straight path from a to b at 'speed' m/s, looking along the motion (optionally at a height offset target).
scene::CameraPath linearPath(std::string name, float3 a, float3 b, float speed, float3 lookOffset = { 0, 0, 0 });
// Orbit around 'centre' at radius/height with angular speed (rad/s) for 'seconds'.
scene::CameraPath orbitPath(std::string name, float3 centre, float radius, float height, float angularSpeed, float seconds);

// City layout shared by CityBlock and CityNight: returns the street-light positions (for CityNight's lamps).
struct CityLayout
{
    std::vector<float3> lampBases;     // foot of every street-light pole
    std::vector<float3> lampHeads;     // lamp position (emitting downward)
    std::vector<float3> windowCentres; // facade window centres with outward normals in windowNormals
    std::vector<float3> windowNormals;
    std::vector<float3> neonCentres;
    std::vector<float3> neonAxes;
    float extent = 0;                  // half size of the city square (m)
};
CityLayout buildCity(Scene& s, const Palette& p, uint64_t seed, float scale, bool night);

// Forest content shared by ForestThin/ForestCard/Waterside.
struct FoliageStyle
{
    bool thin = true;         // true: 6 cm geometric leaves and 4 mm blades; false: 35 cm alpha cards, 30 cm grass cards
};
struct FoliageAssets
{
    std::vector<uint32_t> treeMeshes;   // variants
    std::vector<uint32_t> grassMeshes;  // clump variants
    uint32_t leavesPerTree = 0, bladesPerClump = 0;
};
FoliageAssets buildFoliage(Scene& s, const Palette& p, uint64_t seed, FoliageStyle style);

// HairBall (HairBall.cpp): the diagnostic scene of strand hair; its strands are SceneGen.h's grooms.
Scene hairBall(const Request& rq);

// RPP-1 dynamic bodies (Bodies.cpp): adds the content's bodies to the scene as InstanceDynamic instances and records
// each body's scene instance index.
void addDynamicBodies(Scene& s, DynamicContent& content);
} // namespace unx::scenegen::detail
