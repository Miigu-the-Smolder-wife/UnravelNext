#pragma once
// The mesh card surface cache's CPU side (Docs/Status/MESH_CARDS_INTERFACE_KO.md; owner A): which cards exist, at
// which resolution, where in the atlas, and which pages to capture this frame. No GPU objects: the render pass
// (MeshCards.cpp) uploads these records and rasterises the captures. The rules and numbers are Unreal's Lumen scene
// (LumenMeshCards.cpp, LumenScene.cpp, LumenSceneRendering.cpp), lengths in metres.
#include "unx/core/Math.h"
#include "unx/scene/MeshCards.h"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <utility>
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
    float emissiveMinAreaScale = 0.2f;    // an emissive light source's cards pass the size rule at this share of the
                                          // area (LumenMeshCards::GetCardMinSurfaceArea: x 0.2), and stay visible down
                                          // to a resolution of 1 (bEmissiveLightSource); 1: no such rule
    float refreshFraction = 0.125f;       // CardCaptureRefreshFraction: the share of the frame's captures spent on capturing
                                          // resident pages again (materials that change: animated emission, edits)
    uint32_t maxPages = 262144;           // page table entries (the GPU page buffers' fixed size: atlas texels / the
                                          // smallest allocation's 64)
    // Feedback (r.LumenScene.SurfaceCache.Feedback; LumenSurfaceCacheFeedback.cpp): pages above a card's resident level,
    // mapped where the frame's ray hits asked for more texels than the resident level holds (setFeedback).
    bool feedback = true;
    uint32_t feedbackMinPageHits = 16;    // Feedback.MinPageHits: a page asked for by no more hits than this is not mapped
    uint32_t feedbackTileSize = 16;       // Feedback.TileSize: one hit in a tile of this side reports a frame (a page's
                                          // feedback comes round within its square of frames)
    uint32_t keepUnusedPagesFrames = 256; // NumFramesToKeepUnusedPages: a feedback page no hit asked for this long leaves
    bool operator==(const McSettings&) const = default;
};

// One element of the GPU's feedback (CardLighting.hlsli clFeedback): 'hits' of a frame's reporting ray hits asked page
// (pageX, pageY) of level resLevel of a card.
struct McFeedback
{
    uint32_t card = 0;
    uint8_t resLevel = 0, pageX = 0, pageY = 0;
    uint32_t hits = 0;
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
    bool refresh = false;         // the page was resident and keeps its place: only its geometry atlases are drawn again
};

struct McStats
{
    uint32_t meshCards = 0, cards = 0, visibleCards = 0;
    uint32_t mappedPages = 0, freePhysicalPages = 0;
    uint64_t allocatedTexels = 0, desiredTexels = 0;   // locked levels: what is in the atlas, what the cards ask for
    uint32_t requests = 0;                             // cards whose level differs from what they ask for, and the feedback's
                                                       // pages that are not mapped (before the frame's budget)
    uint32_t captures = 0, capturedTexels = 0;         // this frame
    uint32_t loweredAllocations = 0;                   // this frame: allocated below the requested level (atlas full)
    uint32_t pending = 0;                              // requests the frame's budget left for later frames
    uint32_t refreshed = 0;                            // this frame: resident pages captured again
    uint32_t feedbackElements = 0;                     // this frame: feedback elements with enough hits
    uint32_t hiResRequests = 0;                        // ... of them, pages that are not mapped (before the frame's budget)
    uint32_t hiResMapped = 0;                          // this frame: feedback pages mapped and captured
    uint32_t hiResPages = 0;                           // feedback pages in the atlas (the pages of unlocked levels)
    uint32_t evictedPages = 0;                         // this frame: feedback pages that left (unused, or for room)
    uint32_t feedbackDropped = 0;                      // the newest completed frame's reports that found no place in the
                                                       // GPU's table (set by SurfaceCacheCards; not 0: the table overflowed)
    uint32_t captureOverflow = 0;                      // V's overflow bits of the newest cluster capture run that dropped
                                                       // geometry (set by SurfaceCacheCards; the pages it drew lack it
                                                       // until the refresh comes round)
};

class MeshCardScene
{
public:
    explicit MeshCardScene(const McSettings& settings = {});
    const McSettings& settings() const { return m_settings; }
    void clear();

    // The cards of one instance (mesh space cards of its mesh, the instance's object -> world: rotation, uniform scale,
    // translation). Returns its mesh cards index, mc::kNone when none of its cards passes the size rule.
    // lightingChannels: the instance's (scene::instanceLightingChannels): its cards take the lights that light it.
    // emissiveLightSource (the reference's bEmissiveLightSource): the instance's emission lights the scene - smaller
    // cards are kept (McSettings::emissiveMinAreaScale) and stay visible below the minimum card resolution, so a
    // small lamp's emission is in the cache from as far as its cards reach.
    uint32_t addInstance(uint32_t sceneInstance, const scene::MeshCards& cards, const float3x4& objectToWorld, uint32_t lightingChannels = 1, bool emissiveLightSource = false);
    // A rigid move (same scale): the cards follow, nothing is captured again.
    void setTransform(uint32_t sceneInstance, const float3x4& objectToWorld);
    // The instance's cards leave the atlas and the scene (hidden or removed instance, changed mesh, scale or materials:
    // the caller adds it again and its cards are captured anew).
    void removeInstance(uint32_t sceneInstance);
    bool hasInstance(uint32_t sceneInstance) const { return sceneInstance < m_instanceMap.size() && m_instanceMap[sceneInstance] != mc::kNone; }
    // The instance's resident pages are captured again ahead of the refresh's round (its materials changed); they keep
    // their places and their lighting.
    void refreshInstance(uint32_t sceneInstance);
    uint32_t pageCount() const { return (uint32_t)m_pages.size(); }

    // The page requests of the newest frame whose GPU feedback has come back: once a frame, before the frame's first
    // update (which takes them; it also is the feedback pages' clock - a page's last use is counted in these calls).
    // samples: the hits a frame can report (a request's urgency falls with its share of them). Elements of cards that
    // have left or changed since are harmless: a page mapped for one leaves again unused.
    void setFeedback(std::span<const McFeedback> elements, uint32_t samples);
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
        bool live = false;                    // the slot holds a card of an instance
        float distance = 0;                   // from the nearest view origin, at the last update
        bool visible = false;
        bool projectedVisible = false;        // requested visibility, independent of current atlas allocation
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
        uint32_t lightingChannels = 1;
        bool emissiveLightSource = false;
        bool projectionDirty = true;
    };
    struct PageEntry  // FLumenPageTableEntry
    {
        int32_t card = -1;
        uint8_t resLevel = 0;
        float cardUvRect[4] = {};
        int32_t subAllocX = -1, subAllocY = -1;   // element size of a sub-allocation, -1 = a whole physical page
        int32_t pageCoordX = -1, pageCoordY = -1; // physical page
        uint32_t capturedFrame = 0;               // the frame of its last capture
        uint32_t rect[4] = {};                    // atlas texels: min x, min y, max x, max y
        uint32_t sampleAtlasBiasX = 0, sampleAtlasBiasY = 0, sampleResLevelX = 0, sampleResLevelY = 0, samplePage = 0;
        bool unlocked = false;                    // mapped in a level that is not locked: in m_unlocked under lastUsed
        uint32_t lastUsed = 0;                    // the feedback frame a hit last asked for it (or it was mapped in)
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
            uint32_t firstFree = 0;  // every earlier element is occupied; lowered on free
        };
        struct Bin
        {
            uint32_t elementX = 0, elementY = 0, perSideX = 0, perSideY = 0;
            std::vector<BinPage> pages;
        };
        bool allocatePage(int32_t& x, int32_t& y);
        void freePage(int32_t x, int32_t y);
        uint32_t m_side = 0, m_free = 0, m_firstFree = 0;
        std::vector<uint8_t> m_pages;
        std::map<uint32_t, Bin> m_bins;  // key = elementX | elementY << 16
    };
    struct Request  // FSurfaceCacheRequest
    {
        uint32_t card;
        uint8_t resLevel, distanceBin;
        uint16_t localPage = kLockedMip;  // a feedback request's page of the level; kLockedMip: the card's locked level
    };
    static constexpr uint16_t kLockedMip = 0xFFFFu;

    MipMapDesc mipMapDesc(const Card& card, uint32_t resLevel) const;
    MipMap& mip(Card& card, uint32_t resLevel) { return card.mips[resLevel - mc::kMinResLevel]; }
    const MipMap& mip(const Card& card, uint32_t resLevel) const { return card.mips[resLevel - mc::kMinResLevel]; }
    void updateMinMax(Card& card);
    int32_t addSpan(uint32_t size);
    void removeSpan(int32_t offset, uint32_t size);
    void reallocVirtualSurface(Card& card, uint32_t cardIndex, uint32_t resLevel, bool lock);
    void freeVirtualSurface(Card& card, uint32_t fromLevel, uint32_t toLevel);
    void mapPage(const MipMap& mipMap, uint32_t pageIndex);
    void unmapPage(uint32_t pageIndex);
    void setUnlocked(uint32_t pageIndex, bool unlocked);  // into / out of m_unlocked
    void touchPage(uint32_t pageIndex);                   // a hit asked for an unlocked page: used in this feedback frame
    // FLumenSceneData::EvictOldestAllocation: unmaps the unlocked page used longest ago when that is at least
    // maxFramesSinceLastUsed feedback frames back; false: none is that old.
    bool evictOldest(uint32_t maxFramesSinceLastUsed, std::vector<uint32_t>& dirtyCards);
    void updateMipHierarchy(Card& card);
    void removeCardFromAtlas(uint32_t cardIndex);
    void writeMeshCardsGpu(uint32_t index);
    void writeCardGpu(uint32_t index);
    void writePageGpu(uint32_t index);

    McSettings m_settings;
    uint32_t m_frame = 0;
    bool m_projectionValid = false;
    std::vector<float3> m_projectionOrigins;
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
    std::vector<McFeedback> m_feedback;             // setFeedback's elements, taken by the next update
    uint32_t m_feedbackSamples = 1;
    uint32_t m_feedbackFrame = 0;                   // setFeedback calls so far: the unlocked pages' clock
    std::set<std::pair<uint32_t, uint32_t>> m_unlocked;  // (last used, page index) of every unlocked mapped page, the
                                                    // oldest first (UnlockedAllocationHeap)
    std::vector<uint32_t> m_freeMeshCards;          // removed entries, reused by addInstance
    std::map<uint32_t, uint32_t> m_freeCardSpans;   // offset -> size, coalesced
    uint32_t m_refreshCursor = 0;                   // page index the refresh captures continue from
    std::vector<uint32_t> m_refreshQueue;           // pages asked for by refreshInstance (taken from the back)
    bool spanAvailable(uint32_t size) const;        // room for 'size' more page table entries
    uint32_t addCardSpan(uint32_t size);
    void removeCardSpan(uint32_t offset, uint32_t size);
    void captureOf(uint32_t pageIndex, const PageEntry& page, const PageEntry& capture, uint32_t cardIndex, bool resample, bool refresh);
};
} // namespace unx::render::refl
