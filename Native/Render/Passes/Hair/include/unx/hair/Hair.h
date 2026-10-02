#pragma once
// Strand hair (track E, B10; ARCHITECTURE 2.8, 2.14). Owner: E.
//
// Simulation (per World tick, on the GPU in the frame's simulation work; ARCHITECTURE 2.14 "hair guides"): each body's
// guide strands (nodesPerStrand nodes, the root bound to a joint) advance by `substeps` substeps of
//   1. Verlet integration with gravity, wind (a force per unit length, drag-like) and damping; the root follows its
//      joint (the joint transform interpolated over the tick);
//   2. global shape: every node within globalRange of the root is pulled towards its skinned rest position;
//   3. local shape (TressFX 4): each segment is pulled towards its rest vector in the frame its parent chain carries
//      (the frame rotates with the strand's actual bend), `localIterations` sweeps;
//   4. capsule collision (body capsules: nodes pushed out to radius + margin);
//   5. DFTL (Mueller et al. 2012, dynamic follow-the-leader): lengths restored exactly root to tip, the velocity
//      correction -s d_{i+1} keeps the motion free of the FTL bias (s = dftlDamping);
//   6. capsule collision again (a node moved out may lengthen its segment by at most the push; reported by tests).
// Rendering (per frame): guide states interpolated at the frame time, twist-free frames transported along each guide,
// follow strands = guide nodes + their rest offsets in those frames (tapered from root to tip). The segments go to V's
// coverage layer (band B: exact segment-pixel area) through FrameResources::hairSegments (interface request
// Docs/Design/Requests/20260926_E_hair_strands.md); shading is HairBsdf.hlsli (M's hair class).
// LOD (ARCHITECTURE 2.8): a body whose follow strands would exceed 2 per pixel width keeps a deterministic subset with
// widths scaled by 1 / fraction (projected coverage kept); denser than that is band C's strand bricks (V). The subset is
// the first strands of the body's LOD order (the follows by rising hash), and the frame holds only their segments.
#include "unx/render/Frame.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace unx::hair
{
struct SimulationParams
{
    float3 gravity{ 0, -9.81f, 0 };
    float damping = 0.06f;         // fraction of the Verlet velocity removed per substep
    float globalStiffness = 0.05f; // pull per substep towards the rest pose (0..1)
    float globalRange = 0.3f;      // fraction of the strand (from the root) the global pull acts on
    float localStiffness = 0.6f;   // pull per sweep towards the rest vector in the carried frame (0..1)
    uint32_t localIterations = 3;
    float dftlDamping = 0.9f;      // s of DFTL's velocity correction
    float collisionMargin = 0.002f;
    uint32_t substeps = 2;
    float windDrag = 1.0f;         // wind acceleration per (m/s) of relative air speed
};

struct Capsule
{
    float3 a;
    float radius;
    float3 b;
    uint32_t pad = 0;
};

struct BodyDesc
{
    uint32_t nodesPerStrand = 12;           // <= 32
    uint32_t joints = 1;                    // joints the roots bind to (transforms per tick)
    std::vector<float3> restPositions;      // guides x nodes, in the space of the guide's joint
    std::vector<uint32_t> guideJoint;       // per guide
    struct Follow
    {
        uint32_t guide;
        float3 offset;                      // at the root, in the guide's rest frame (x along the root segment)
        float tipSpread = 1.0f;             // offset scale at the tip (1: parallel)
    };
    std::vector<Follow> follows;
    float rootRadius = 40e-6f, tipRadius = 20e-6f;  // metres (human hair ~ 60-100 um diameter)
    uint32_t material = 0;                  // scene material (hair class)
    uint32_t instance = 0xFFFFFFFFu;        // scene instance the hair belongs to (shading, identity)
    SimulationParams params;
};

class HairSystem
{
public:
    HairSystem();
    ~HairSystem();
    uint32_t addBody(const BodyDesc& desc);
    void removeBody(uint32_t body);
    // One World tick: the joints' world transforms at the tick's end (the previous tick's are kept), the body's
    // capsules (world, at the tick's end), the wind velocity at the body. dt: the tick interval.
    void tick(uint32_t body, const float3x4* joints, uint32_t jointCount, const Capsule* capsules, uint32_t capsuleCount, float3 wind, float dt);
    // The frame's time within the latest tick (0 = previous tick's end, 1 = latest tick's end).
    void setFrameFraction(float w) { m_fraction = w; }

    struct Body;
    std::vector<Body*> bodies();
    float fraction() const { return m_fraction; }
    // The body's guide states at the latest tick's end (float4 per node, world), valid in the frame after
    // tracks::hair (tests read it back).
    render::BufferRef tickState(uint32_t body) const;

private:
    std::vector<std::unique_ptr<Body>> m_bodies;
    std::vector<uint32_t> m_free;
    float m_fraction = 1.0f;
};

HairSystem& hairSystem(render::TrackState& state);

// Per-frame outputs (FrameResources::hairSegments / hairBodies): the segments of every drawn follow strand, 2 float4 each
// (camera-relative p0, r0), (p1, r1) - a body's strands in the follows' order when all are drawn, else the kept ones in
// its LOD order, side by side (strands left out by the LOD have no segments); the bodies' header (raw): body count, then
// per body 8 words { first segment, segments (of the strands drawn), segments per strand, material, instance, LOD keep
// fraction (float), width scale (float), 0 }.
constexpr uint32_t kBodyHeaderWords = 8;
} // namespace unx::hair
