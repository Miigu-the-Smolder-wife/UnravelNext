#pragma once
// Mesh card generation off the frame: the cards of a mesh at a scale (scene::buildMeshCards) are made once on worker
// threads and kept on disk by the content of the mesh, so a level's first load pays the generation (the lobby 1.9 s of
// worker time, a forest of pines 98 s) and later loads read a few bytes per mesh. The frame never waits: a mesh whose
// cards are not ready has none yet and gets them in the frame after they arrive.
#include "unx/scene/MeshCards.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace unx::render::refl
{
class MeshCardCache
{
public:
    // 'directory': where generated cards are kept (created on first use); empty = no disk cache.
    explicit MeshCardCache(std::filesystem::path directory);
    ~MeshCardCache();
    MeshCardCache(const MeshCardCache&) = delete;
    MeshCardCache& operator=(const MeshCardCache&) = delete;

    // The cards of scene mesh 'mesh' shown at 'scale' (its instances' uniform scale), or null while they are being made
    // (the request is queued on the first call). The pointer stays valid until clear(). 'identity': a value that changes
    // when the mesh's content does (the scene's upload count), so a reused mesh index is not taken for the old mesh.
    const scene::MeshCards* find(const scene::Scene& scene, uint32_t mesh, float scale, uint64_t identity);
    // Requests not finished yet.
    uint32_t pending() const;
    // Blocks until every queued request has finished (tools, tests, a load screen that wants the cards before frame 1).
    void wait();
    // Drops every entry (waits for running jobs first).
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// The default card directory: %LOCALAPPDATA%/UnravelNext/MeshCards (the temp directory without LOCALAPPDATA).
std::filesystem::path defaultMeshCardDirectory();
} // namespace unx::render::refl
