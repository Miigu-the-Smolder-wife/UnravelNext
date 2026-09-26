#pragma once
// Surface state field on the GPU (track E, A7; FEATURES_GAME 5.2, WORLD_VFX 10.4). Owner: E. See SurfaceState.hlsli.
//
// The host feeds the VFX context's surface state as deltas (NativeVfx nv_surface_delta: changed bricks in the
// NV_SurfaceBrickV2 layout and removed keys) and the half-lives; each frame gives the VFX time of the frame
// (setTime). The field keeps the renderer's copy: a pool of 464 B bricks (unorm8 x 5 + int16 snow, quantized here;
// the VFX authority stays float) and an open-addressing hash table (linear probing, backward-shift deletion), and
// tracks::surfaceState uploads only what changed (brick records and table entries, one scatter dispatch each) plus the
// 48 B constants. M's resolve samples it with surfaceStateAt (a lookup per brick met + trilinear over voxel centres).
// Memory: surface.max_bricks x 464 B pool + 2 x that many 16 B table entries on the GPU, the same records on the CPU
// (65,536 bricks: 30.4 + 2.1 MB each side).
#include "unx/render/Frame.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace unx::surface
{
constexpr uint32_t kChannels = 6;      // wet, scorch, frost, dust, blood, snow
constexpr uint32_t kBrickBytes = 464;  // SURFACE_BRICK_BYTES
constexpr uint32_t kEmpty = 0xFFFFFFFFu;

// NativeVfx NV_SurfaceBrickV2 (same bytes): value[v * 6 + c], voxel v = (i * 4 + j) * 4 + k.
struct BrickInput
{
    int32_t key[3];
    uint32_t reserved;
    double t0;
    float value[64 * kChannels];
};
static_assert(sizeof(BrickInput) == 1560);

uint32_t hash(int32_t x, int32_t y, int32_t z);  // = surfaceHash (SurfaceState.hlsli)
// The quantized GPU record of a brick: key, t0 (float seconds), unorm8 x 5 per voxel, int16 snow (1/2048 m) per voxel.
void encode(const BrickInput& in, uint8_t out[kBrickBytes]);

class SurfaceField
{
public:
    // Removed keys first, then changed bricks (new or replaced). Fails past capacity (surface.max_bricks).
    void apply(const BrickInput* changed, size_t changedCount, const int32_t* removedKeys, size_t removedCount);
    void setHalfLives(const std::array<double, kChannels>& halfLife);  // seconds, 0 = no decay
    void setTime(double seconds) { m_now = seconds; }                  // the frame's VFX context time
    void clear();

    size_t bricks() const { return m_slotOf.size(); }
    uint32_t maxProbe() const { return m_maxProbe; }
    double now() const { return m_now; }
    const std::array<double, kChannels>& halfLives() const { return m_halfLife; }

    // Upload interface (tracks::surfaceState).
    void setCapacity(uint32_t maxBricks);  // a new capacity rebuilds the table and uploads everything
    uint32_t capacity() const { return m_capacity; }
    uint32_t tableSize() const { return (uint32_t)m_table.size(); }
    const std::vector<std::array<int32_t, 4>>& table() const { return m_table; }
    const std::vector<uint8_t>& records() const { return m_records; }  // capacity x kBrickBytes
    std::vector<uint32_t> takeDirtySlots();
    std::vector<uint32_t> takeDirtyEntries();
    bool takeWhole();  // true once after a capacity change: upload the whole pool and table
    std::vector<uint32_t> liveSlots() const;

private:
    struct Key
    {
        int32_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const { return hash(k.x, k.y, k.z); }
    };
    void insert(const Key& k, uint32_t slot);
    void erase(const Key& k);
    void dirtyEntry(uint32_t i);

    uint32_t m_capacity = 0;
    std::vector<std::array<int32_t, 4>> m_table;  // x, y, z, slot (kEmpty = empty)
    std::vector<uint8_t> m_records;
    std::unordered_map<Key, uint32_t, KeyHash> m_slotOf;
    std::vector<uint32_t> m_free;
    uint32_t m_next = 0, m_maxProbe = 0;
    std::array<double, kChannels> m_halfLife{ 60, 0, 180, 0, 0, 0 };
    double m_now = 0;
    bool m_whole = true;
    std::vector<uint8_t> m_slotDirty, m_entryDirty;
    std::vector<uint32_t> m_dirtySlots, m_dirtyEntries;
};

SurfaceField& surfaceField(render::TrackState& state);
} // namespace unx::surface
