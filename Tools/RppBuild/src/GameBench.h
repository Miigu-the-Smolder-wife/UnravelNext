#pragma once
// Game bench scene (user decision 2026-09-26, relayed by coordination): the user's games are first-person, physics-heavy and
// graphically light. The bench is parametric: moving physics objects (250 / 500 / 1000 at peak, every kind: rigid boxes and
// clutter, ragdolls, vehicles, hinges, cloth, ropes, soft bodies, fracturable objects with a debris cap, Matter fluid), a
// yard with plain ground, buildings and tens of lights with standard materials, and a first-person camera track (walk,
// sprint, strafe, look, fast turns) with a view-model slot. Spec: Content/RPP1/GameBench/GAMEBENCH_KO.md.
//
// Outputs: <name>.unxscene (environment + the rigid objects as dynamic instances), <name>_bodies.json (C bodies format 1:
// the rigid subset with kinematic motions, so the native host gate renders the bench without a physics engine) and
// <name>.json (parameters, the full population for the physics engine, destruction events, camera track, result schema).
#include "unx/scene/SceneData.h"

#include <string>
#include <vector>

namespace unx::rpp
{
struct GameBenchMix  // shares of the spawned objects (sum 1)
{
    float box = 0.30f, clutter = 0.30f, fracturable = 0.20f;
    float ragdoll = 0.05f, vehicle = 0.02f, hinge = 0.03f;
    float cloth = 0.04f, rope = 0.04f, soft = 0.02f;
};

struct GameBenchRequest
{
    std::string name = "fp_1000";
    uint64_t seed = 1;
    uint32_t movingObjects = 1000;     // peak concurrently moving objects, debris included (user: <= 1000)
    float debrisShare = 0.25f;         // debris cap = movingObjects x share; spawned objects = movingObjects - cap
    GameBenchMix mix;
    float destructionPerMinute = 12;   // fracture events (explosions / impacts), evenly jittered
    uint32_t fragmentsPerBreak = 16;   // fragments of one fractured object (the debris cap bounds the live total)
    float debrisLifetime = 20;         // s; the oldest fragments despawn first when the cap is reached
    uint32_t fluidParticles = 0;       // Matter fluid in the pond (0, 65536, 262144 ...)
    float durationSeconds = 68;        // camera cases end with the water case at 68 s
    uint32_t lights = 24, shadowedLights = 8;
    float verticalFovDeg = 59.0f;      // ~90 deg horizontal at 16:9 (first-person)
};

struct GameBenchOutput
{
    scene::Scene scene;
    std::string bodiesJson;  // format 1, rigid subset
    std::string benchJson;   // parameters, population, events, camera, result schema
    uint32_t spawned = 0, debrisCap = 0, rigidReplayed = 0;
};

GameBenchOutput buildGameBench(const GameBenchRequest& request);
} // namespace unx::rpp
