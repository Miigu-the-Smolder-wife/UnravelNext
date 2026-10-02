#pragma once
// Projected decals (track E, A7; FEATURES_GAME 5). Owner: E. See Passes/Decal/Decal.hlsli for the rules.
//
// The game adds decals to the renderer's set (decal::decals(renderer.trackState())): an oriented box and a scene
// material, world space or attached to an instance. Each frame, tracks::decals (after V, before M's resolve) builds
// the camera-relative records and the 16 x 16 px tile lists into ViewResources::decalFrames / decalTiles; M's resolve
// (and R's hit shading) call decalApply. Cost [FEATURES_GAME 5.3, expected]: setup O(decals), tile depth one pass over
// the depth buffer, culling O(decals x tiles of their screen rectangles), apply (M) O(covered pixels x decals there).
#include "unx/render/Frame.h"

#include <cstdint>
#include <vector>

namespace unx::decal
{
constexpr uint32_t kNone = 0xFFFFFFFFu;

// The parts of the receiver's material a decal changes (Decal::channels; Decal.hlsli DECAL_CHANNEL_*). Unreal names them
// by the decal material's connected outputs: a normal-only decal, a roughness-only decal.
enum DecalChannels : uint32_t
{
    DecalBaseColor = 1,
    DecalNormal = 2,      // the normal and its slope variance
    DecalRoughMetal = 4,  // roughness and metallic
    DecalAllChannels = 7,
};

struct Decal
{
    float3x4 box;                 // unit cube [-1, 1]^3 -> space: columns = half-extent axes X, Y, Z, then the centre
    uint32_t material = 0;        // scene material (its textures: base colour x alpha = opacity, rough/metal, normal)
    uint32_t instance = kNone;    // kNone: world space; else the instance's object space (moves with it, paints only it)
    int32_t priority = 0;         // composition order: priority, then creation order
    float opacity = 1.0f;
    float fadeStartDegrees = 60;  // angle between the surface's geometric normal and the box's +Z: full up to start,
    float fadeEndDegrees = 80;    // none from end on
    float edge = 0.25f;           // soft fraction of the box depth at its +-Z faces (0: hard)
    float3 color = { 1, 1, 1 };   // tint of the decal's base colour (Unreal's decal colour)
    uint32_t channels = DecalAllChannels;  // DecalChannels the decal changes
    // Fade with the decal's size on screen (Unreal's FadeScreenSize; 0: none): with screen = the box's largest half
    // extent / its distance and k = fadeScreenSize x 2 tan(half fov x) / view width x 600, the opacity is times
    // saturate((screen - k) / (k / 2)) - gone below k, full from 1.5 k.
    float fadeScreenSize = 0;
    // Lifetime fades on the frame's clock (FrameContext::time, s): in over [fadeInStart, fadeInStart + fadeInDuration],
    // out over [fadeOutStart, fadeOutStart + fadeOutDuration]; a duration of 0: no such fade. A decal that has faded
    // out stays in the set (it costs its frame record, no tile entries) until the game removes it.
    float fadeInStart = 0, fadeInDuration = 0, fadeOutStart = 0, fadeOutDuration = 0;
};

class DecalSet
{
public:
    uint32_t add(const Decal& d);            // returns the decal's id
    void update(uint32_t id, const Decal& d);  // keeps its creation order
    void remove(uint32_t id);
    void clear();
    uint32_t count() const { return m_live; }
    uint64_t revision() const { return m_revision; }

    // GPU records of the live decals (dense), as Decal.hlsli DecalRecord (128 B each).
    std::vector<uint8_t> records() const;

private:
    struct Slot
    {
        Decal decal;
        uint32_t order = 0;
        bool live = false;
    };
    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_free;
    uint32_t m_live = 0, m_nextOrder = 0;
    uint64_t m_revision = 1;
};

// The renderer's set (FrameRenderer::trackState()).
DecalSet& decals(render::TrackState& state);

// Tile-list statistics of the last frame read back (framesInFlight frames late).
struct Stats
{
    uint64_t frame = UINT64_MAX;
    uint32_t decals = 0;
    uint32_t fullTiles = 0;  // tiles that met more than DECAL_PER_TILE decals (the extra ones are not drawn there)
};
Stats lastStats(render::TrackState& state);
} // namespace unx::decal
