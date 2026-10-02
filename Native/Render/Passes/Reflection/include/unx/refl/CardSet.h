#pragma once
// A mesh card set as S2's card lighting reads it (Docs/Status/MESH_CARDS_INTERFACE_KO.md sections 2-4; the shader side
// is Passes/SurfaceCache/CardLayout.hlsli). The real set is A's (unx/refl/MeshCards.h: generation, capture, residency);
// until that lands the only producer is CardTestSet below.
#include "unx/render/Frame.h"

#include <memory>

namespace unx::render::refl
{
struct CardSet
{
    bool valid = false;
    BufferRef instanceMap, meshCards, cards, cardPages, pageTable;  // raw
    TextureRef depth, albedo, normal, emissive;                     // atlasSize^2
    uint32_t atlasSize = 0;
    uint32_t cardPageCapacity = 0;  // S2's per-page state is sized by it
    uint32_t cardPageCount = 0;     // pages in use (indices below it)
    uint32_t instances = 0;         // entries of instanceMap
    uint64_t generation = 0;        // changes when the set was rebuilt from nothing (S2's lighting starts over)
};

// surface_cache.mesh_cards_test_set: a hand-made card set for S2's own checks (the furnace, a box room) before A's
// capture exists. Built on the CPU from the scene's source data at each scene revision: per rigid instance up to six
// cards - one per axis direction over the mesh's bounds - each one page of at most 128 x 128 texels at 20 texels a metre,
// filled by an orthographic raster of the mesh's triangles that face the direction (n . d >= 0.25, the nearest to the
// card's front kept), with the material's constants (no textures). No residency, no feedback, no update when an
// instance moves. Not a renderer path: a fixture.
class CardTestSet
{
public:
    explicit CardTestSet(Device& device);
    ~CardTestSet();
    // Builds (first call, scene revision) and uploads; returns the set's graph references for this frame.
    CardSet record(FramePassContext& fc);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace unx::render::refl
