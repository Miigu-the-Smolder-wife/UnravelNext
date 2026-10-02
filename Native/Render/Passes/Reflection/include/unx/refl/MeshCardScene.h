#pragma once
// The mesh card surface cache's CPU side (Docs/Status/MESH_CARDS_INTERFACE_KO.md; owner A): which cards exist, at
// which resolution, where in the atlas, and which pages to capture this frame. No GPU objects: the render pass
// (MeshCards.cpp) uploads these records and rasterises the captures. The rules and numbers are Unreal's Lumen scene
// (LumenMeshCards.cpp, LumenScene.cpp, LumenSceneRendering.cpp), lengths in metres.
#include "unx/core/Math.h"
#include "unx/scene/MeshCards.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace unx::render::refl
{
namespace mc
{
constexpr uint32_t kPhysicalPage = 128;     // Lumen::PhysicalPageSize
constexpr uint32_t kVirtualPage = 127;      // 0.5 texel border around a page
constexpr uint32_t kMinResLevel = 3;        // 8 texels
constexpr uint32_t kMaxResLevel = 11;       // 2048 texels
constexpr uint32_t kSubAllocResLevel = 7;   // log2(kPhysicalPage): smaller levels share a physical page
constexpr uint32_t kResLevels = kMaxResLevel - kMinResLevel + 1;
constexpr uint32_t kTile = 8;               // Lumen::CardTileSize
constexpr uint32_t kDistanceBuckets = 16;
constexpr uint32_t kMaxCardsPerMesh = 32;   // the card lookup is a 32-bit mask
constexpr uint32_t kNone = 0xFFFFFFFFu;
} // namespace mc

struct McSettings
{
    uint32_t atlasSize = 4096;            // r.LumenScene.SurfaceCache.AtlasSize
    uint32_t capturesPerFrame = 300;      // CardCapturesPerFrame (pages)
    uint32_t captureFactor = 64;          // CardCaptureFactor: texels per frame = atlas texels / factor
    float texelDensityScale = 100.0f;     // CardTexelDensityScale
    float maxTexelDensity = 20.0f;        // CardMaxTexelDensity 0.2 / cm, in texels per metre
    uint32_t maxResolution = 512;         // CardMaxResolution
    uint32_t minResolution = 4;           // CardMinResolution: below it the card is hidden
    float maxDistance = 300.0f;           // card range from the camera (r.RayTracing.Culling.Radius 30000)
    float minSize = 0.1f;                 // MeshCardsMinSize 10
    bool operator==(const McSettings&) const = default;
};

// GPU records (Passes/SurfaceCache/MeshCards.hlsli).
struct McMeshCardsGpu  // 80 B
{
    float worldToLocal[3][4];
    uint32_t cardOffset, countFlags;
    uint32_t cardLookup[6];
};
static_assert(sizeof(McMeshCardsGpu) == 80);
struct McCardGpu  // 112 B
{
    float origin[3];
    uint32_t packed;
    float extent[3];
    float texelSize;
    uint32_t sizeInPages, pageTableOffset, hiResSizeInPages, hiResPageTableOffset;
    float cardToWorld[3][4];
    uint32_t meshCards, pad[3];
};
static_assert(sizeof(McCardGpu) == 112);
struct McCardPageGpu  // 64 B
{
    uint32_t card, resLevelPageTableOffset;
    float sizeInTexels[2];
    float cardUvRect[4];
    float atlasRect[4];
    float cardUvTexelScale[2];
    uint32_t resLevelSizeInTiles, pad;
};
static_assert(sizeof(McCardPageGpu) == 64);
struct McPageTableGpu  // 8 B
{
    uint32_t packed, cardPage;
};

// One page to rasterise this frame.
struct McCapture
{
    uint32_t page = 0;            // page table index = card page index
    uint32_t card = 0;
    uint32_t sceneInstance = 0;
    uint32_t captureRect[4] = {}; // x, y, width, height in the capture atlas
    uint32_t atlasRect[4] = {};   // x, y, width, height in the atlas
    float cardUvRect[4] = {};     // the card uv the page covers (min xy, max zw)
    bool resample = false;        // the card had pages before this frame (its lighting can be carried over)
};

struct McStats
{
    uint32_t meshCards = 0, cards = 0, visibleCards = 0;
    uint32_t mappedPages = 0, freePhysicalPages = 0;
    uint64_t allocatedTexels = 0, desiredTexels = 0;   // locked levels: what is in the atlas, what the cards ask for
    uint32_t requests = 0;                             // cards whose level differs from what they ask for (before the frame's budget)
    uint32_t captures = 0, capturedTexels = 0;         // this frame
    uint32_t loweredAllocations = 0;                   // this frame: allocated below the requested level (atlas full)
};

class MeshCardScene
{
public:
    explicit MeshCardScene(const McSettings& settings = {});
    const McSettings& settings() const { return m_settings; }
    void clear();

    // The cards of one instance (mesh space cards of its mesh, the instance's object -> world: rotation, uniform scale,
    // translation). Returns its mesh cards index, mc::kNone when none of its cards passes the size rule.
    uint32_t addInstance(uint32_t sceneInstance, const scene::MeshCards& cards, const float3x4& objectToWorld);
    // A rigid move (same scale): the cards follow, nothing is captured again.
    void setTransform(uint32_t sceneInstance, const float3x4& objectToWorld);

    // One frame: resolutions from the view origins, allocation, the frame's captures.
    void update(std::span<const float3> viewOrigins);
    const std::vector<McCapture>& captures() const { return m_captures; }
    uint32_t frame() const { return m_frame; }
    const McStats& stats() const { return m_stats; }
    uint32_t captureAtlasSize() const { return m_captureAtlasPages * mc::kPhysicalPage; }

    // GPU mirrors. instanceMap()[scene instance] = mesh cards index or mc::kNone.
    const std::vector<uint32_t>& instanceMap() const { return m_instanceMap; }
    const std::vector<McMeshCardsGpu>& meshCardsGpu() const { return m_meshCardsGpu; }
    const std::vector<McCardGpu>& cardsGpu() const { return m_cardsGpu; }
    const std::vector<McCardPageGpu>& pagesGpu() const { return m_pagesGpu; }
    const std::vector<McPageTableGpu>& pageTableGpu() const { return m_pageTableGpu; }
    // Elements written since the last call (sorted, unique).
    struct Dirty
    {
        std::vector<uint32_t> meshCards, cards, pages;
        bool instanceMap = false;
    };
    Dirty takeDirty();

    // Checks the allocation state (mapped pages inside the atlas, no two overlapping, spans consistent); throws
    // unx::Error on the first problem. For tests.
    void validate() const;

private:
    struct MipMap  // FLumenSurfaceMipMap
    {
        int32_t pageTableOffset = -1;
        uint16_t pageTableSize = 0;
        uint8_t sizeInPagesX = 0, sizeInPagesY = 0, resLevelX = 0, resLevelY = 0;
        bool locked = false;
        bool allocated() const { return pageTableSize > 0; }
    };
    struct MipMapDesc
    {
        uint32_t resolutionX, resolutionY, sizeInPagesX, sizeInPagesY, pageResolutionX, pageResolutionY, resLevelX, resLevelY;
        bool subAllocation;
    };
    struct Card
    {
        uint32_t meshCards = 0;
        uint8_t direction = 0, biasX = 0, biasY = 0;
        float3 origin, extent;                // mesh cards space: centre, card-axis half sizes (scaled metres)
        float3 boxExtent;                     // the same half sizes along the mesh axes
        bool visible = false;
        uint8_t desiredLockedResLevel = 0, desiredLockedResLevelOnLastAlloc = 0;
        uint8_t minAllocatedResLevel = 0xFF, maxAllocatedResLevel = 0;
        MipMap mips[mc::kResLevels];
        bool allocated() const { return minAllocatedResLevel <= maxAllocatedResLevel; }
    };
    struct MeshCardsEntry
    {
        uint32_t sceneInstance = 0, firstCard = 0, cardCount = 0;
        float3x4 objectToWorld;
        float scale = 1;
        float rotation[3][3] = {};  // columns = the mesh axes in world space (unit)
        bool mostlyTwoSided = false;
    };
    struct PageEntry  // FLumenPageTableEntry
    {
        int32_t card = -1;
        uint8_t resLevel = 0;
        float cardUvRect[4] = {};
        int32_t subAllocX = -1, subAllocY = -1;   // element size of a sub-allocation, -1 = a whole physical page
        int32_t pageCoordX = -1, pageCoordY = -1; // physical page
        uint32_t rect[4] = {};                    // atlas texels: min x, min y, max x, max y
        uint32_t sampleAtlasBiasX = 0, sampleAtlasBiasY = 0, sampleResLevelX = 0, sampleResLevelY = 0, samplePage = 0;
        bool mapped() const { return pageCoordX >= 0; }
        bool subAllocation() const { return subAllocX >= 0; }
    };
    // FLumenSurfaceCacheAllocator: physical pages, and bins of equally sized elements inside shared pages.
    class Allocator
    {
    public:
        void init(uint32_t pagesPerSide);
        bool spaceAvailable(const MipMapDesc& desc, bool singlePage) const;
        void allocate(PageEntry& page);
        void free(PageEntry& page);
        uint32_t freePages() const { return m_free; }

    private:
        struct BinPage
        {
            int32_t x, y;
            std::vector<uint8_t> used;
            uint32_t usedCount = 0;
        };
        struct Bin
        {
            uint32_t elementX = 0, elementY = 0, perSideX = 0, perSideY = 0;
            std::vector<BinPage> pages;
        };
        bool allocatePage(int32_t& x, int32_t& y);
        void freePage(int32_t x, int32_t y);
        uint32_t m_side = 0, m_free = 0;
        std::vector<uint8_t> m_pages;
        std::map<uint32_t, Bin> m_bins;  // key = elementX | elementY << 16
    };
    struct Request  // FSurfaceCacheRequest (locked levels)
    {
        uint32_t card;
        uint8_t resLevel, distanceBin;
    };

    MipMapDesc mipMapDesc(const Card& card, uint32_t resLevel) const;
    MipMap& mip(Card& card, uint32_t resLevel) { return card.mips[resLevel - mc::kMinResLevel]; }
    const MipMap& mip(const Card& card, uint32_t resLevel) const { return card.mips[resLevel - mc::kMinResLevel]; }
    void updateMinMax(Card& card);
    int32_t addSpan(uint32_t size);
    void removeSpan(int32_t offset, uint32_t size);
    void reallocVirtualSurface(Card& card, uint32_t cardIndex, uint32_t resLevel, bool lock);
    void freeVirtualSurface(Card& card, uint32_t fromLevel, uint32_t toLevel);
    void mapPage(const MipMap& mipMap, uint32_t pageIndex);
    void unmapPage(PageEntry& page);
    void updateMipHierarchy(Card& card);
    void removeCardFromAtlas(uint32_t cardIndex);
    void writeMeshCardsGpu(uint32_t index);
    void writeCardGpu(uint32_t index);
    void writePageGpu(uint32_t index);

    McSettings m_settings;
    uint32_t m_frame = 0;
    uint32_t m_captureAtlasPages = 4;
    std::vector<MeshCardsEntry> m_meshCards;
    std::vector<Card> m_cards;
    std::vector<PageEntry> m_pages;                // the page table
    std::map<int32_t, uint32_t> m_freeSpans;       // offset -> size, coalesced
    Allocator m_allocator;
    std::vector<McCapture> m_captures;
    McStats m_stats;
    std::vector<uint32_t> m_instanceMap;
    std::vector<McMeshCardsGpu> m_meshCardsGpu;
    std::vector<McCardGpu> m_cardsGpu;
    std::vector<McCardPageGpu> m_pagesGpu;
    std::vector<McPageTableGpu> m_pageTableGpu;
    Dirty m_dirty;
    std::vector<Request> m_requests;
};
} // namespace unx::render::refl
