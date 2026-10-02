// The mesh card surface cache's CPU side (unx/refl/MeshCardScene.h). Function by function this follows Unreal's Lumen
// scene: AddMeshCardsFromBuildData / FLumenCard (LumenMeshCards.cpp), FLumenSurfaceCacheAllocator, MapSurfaceCachePage,
// ReallocVirtualSurface, FreeVirtualSurface, UpdateCardMipMapHierarchy, EvictOldestAllocation (LumenScene.cpp), the
// per-frame update: FLumenSurfaceCacheUpdateMeshCardsTask and ProcessLumenSurfaceCacheRequests (LumenSceneRendering.cpp),
// and the feedback's requests: UpdateSurfaceCacheFeedback (LumenSurfaceCacheFeedback.cpp). A card keeps its locked
// (always resident) level, set by its distance; above it, single pages of higher levels are mapped where the frame's
// ray hits asked for them (setFeedback) and leave again when no hit has asked for them for a while, the one used
// longest ago first.
#include "unx/refl/MeshCardScene.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace unx::render::refl
{
namespace
{
uint32_t floorLog2(uint32_t v)
{
    uint32_t l = 0;
    while (v > 1)
    {
        v >>= 1;
        ++l;
    }
    return l;
}
uint32_t roundUpPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}
float comp(float3 v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Distance bin of a request (Lumen::GetMeshCardDistanceBin, cm): everything within 10 m is bin 0.
uint32_t distanceBin(float metres)
{
    const int offset = std::max(1, (int)((metres - 10.0f) * 100.0f));
    return std::min(floorLog2((uint32_t)offset), mc::kDistanceBuckets - 1);
}

void markDirty(std::vector<uint32_t>& list, uint32_t index) { list.push_back(index); }
void sortUnique(std::vector<uint32_t>& list)
{
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
}
} // namespace

// ---- allocator --------------------------------------------------------------------------------------------------------
void MeshCardScene::Allocator::init(uint32_t pagesPerSide)
{
    m_side = pagesPerSide;
    m_free = pagesPerSide * pagesPerSide;
    m_pages.assign(m_free, 0);
    m_bins.clear();
}

bool MeshCardScene::Allocator::allocatePage(int32_t& x, int32_t& y)
{
    for (uint32_t i = 0; i < m_pages.size(); ++i)
        if (m_pages[i] == 0)
        {
            m_pages[i] = 1;
            --m_free;
            x = (int32_t)(i % m_side);
            y = (int32_t)(i / m_side);
            return true;
        }
    return false;
}

void MeshCardScene::Allocator::freePage(int32_t x, int32_t y)
{
    m_pages[(size_t)y * m_side + (size_t)x] = 0;
    ++m_free;
}

bool MeshCardScene::Allocator::spaceAvailable(const MipMapDesc& desc, bool singlePage) const
{
    const uint32_t wanted = singlePage ? 1u : desc.sizeInPagesX * desc.sizeInPagesY;
    if (m_free >= wanted) return true;
    // no free page, but maybe a free element in a bin of this size
    if (desc.subAllocation)
    {
        const auto bin = m_bins.find(desc.resolutionX | desc.resolutionY << 16);
        if (bin != m_bins.end())
            for (const BinPage& p : bin->second.pages)
                if (p.usedCount < p.used.size()) return true;
    }
    return false;
}

void MeshCardScene::Allocator::allocate(PageEntry& page)
{
    if (page.subAllocation())
    {
        Bin& bin = m_bins[(uint32_t)page.subAllocX | (uint32_t)page.subAllocY << 16];
        if (bin.elementX == 0)
        {
            bin.elementX = (uint32_t)page.subAllocX;
            bin.elementY = (uint32_t)page.subAllocY;
            bin.perSideX = mc::kPhysicalPage / bin.elementX;
            bin.perSideY = mc::kPhysicalPage / bin.elementY;
        }
        BinPage* target = nullptr;
        for (BinPage& p : bin.pages)
            if (p.usedCount < p.used.size())
            {
                target = &p;
                break;
            }
        if (!target)
        {
            BinPage p;
            if (!allocatePage(p.x, p.y)) return;  // (the caller checked spaceAvailable)
            p.used.assign((size_t)bin.perSideX * bin.perSideY, 0);
            bin.pages.push_back(std::move(p));
            target = &bin.pages.back();
        }
        for (uint32_t e = 0; e < target->used.size(); ++e)
            if (target->used[e] == 0)
            {
                target->used[e] = 1;
                ++target->usedCount;
                page.pageCoordX = target->x;
                page.pageCoordY = target->y;
                page.rect[0] = (uint32_t)target->x * mc::kPhysicalPage + (e % bin.perSideX) * bin.elementX;
                page.rect[1] = (uint32_t)target->y * mc::kPhysicalPage + (e / bin.perSideX) * bin.elementY;
                page.rect[2] = page.rect[0] + bin.elementX;
                page.rect[3] = page.rect[1] + bin.elementY;
                return;
            }
    }
    else
    {
        int32_t x = -1, y = -1;
        if (!allocatePage(x, y)) return;
        page.pageCoordX = x;
        page.pageCoordY = y;
        page.rect[0] = (uint32_t)x * mc::kPhysicalPage;
        page.rect[1] = (uint32_t)y * mc::kPhysicalPage;
        page.rect[2] = page.rect[0] + mc::kPhysicalPage;
        page.rect[3] = page.rect[1] + mc::kPhysicalPage;
    }
}

void MeshCardScene::Allocator::free(PageEntry& page)
{
    if (page.subAllocation())
    {
        const auto it = m_bins.find((uint32_t)page.subAllocX | (uint32_t)page.subAllocY << 16);
        if (it == m_bins.end()) return;
        Bin& bin = it->second;
        for (size_t i = 0; i < bin.pages.size(); ++i)
        {
            BinPage& p = bin.pages[i];
            if (p.x != page.pageCoordX || p.y != page.pageCoordY) continue;
            const uint32_t ex = (page.rect[0] - (uint32_t)p.x * mc::kPhysicalPage) / bin.elementX;
            const uint32_t ey = (page.rect[1] - (uint32_t)p.y * mc::kPhysicalPage) / bin.elementY;
            p.used[(size_t)ey * bin.perSideX + ex] = 0;
            if (--p.usedCount == 0)
            {
                freePage(p.x, p.y);
                bin.pages.erase(bin.pages.begin() + (std::ptrdiff_t)i);
            }
            return;
        }
    }
    else
    {
        freePage(page.pageCoordX, page.pageCoordY);
    }
}

// ---- scene ------------------------------------------------------------------------------------------------------------
MeshCardScene::MeshCardScene(const McSettings& settings) : m_settings(settings) { clear(); }

void MeshCardScene::clear()
{
    if (m_settings.atlasSize == 0 || m_settings.atlasSize % mc::kPhysicalPage != 0 || m_settings.atlasSize > 0x1000u * 8u)
        throw Error("mesh cards: the atlas size must be a multiple of " + std::to_string(mc::kPhysicalPage) + " and at most 32768 (" +
                    std::to_string(m_settings.atlasSize) + ")");
    m_frame = 0;
    m_meshCards.clear();
    m_cards.clear();
    m_pages.clear();
    m_freeSpans.clear();
    m_allocator.init(m_settings.atlasSize / mc::kPhysicalPage);
    // FLumenSceneData::GetCardCaptureAtlasSizeInPages: the atlas side / sqrt(capture factor), at least the largest card
    const float perSide = 1.0f / std::sqrt(std::clamp((float)m_settings.captureFactor, 1.0f, 1024.0f));
    const uint32_t pages = ((uint32_t)((float)m_settings.atlasSize * perSide + 0.5f) + mc::kPhysicalPage - 1) / mc::kPhysicalPage;
    const uint32_t cardMaxPages = (m_settings.maxResolution + mc::kPhysicalPage - 1) / mc::kPhysicalPage;
    m_captureAtlasPages = std::clamp(pages, cardMaxPages, m_settings.atlasSize / mc::kPhysicalPage);
    m_captures.clear();
    m_stats = {};
    m_instanceMap.clear();
    m_meshCardsGpu.clear();
    m_cardsGpu.clear();
    m_pagesGpu.clear();
    m_pageTableGpu.clear();
    m_dirty = {};
    m_dirty.instanceMap = true;
    m_freeMeshCards.clear();
    m_freeCardSpans.clear();
    m_refreshCursor = 0;
    m_refreshQueue.clear();
    m_feedback.clear();
    m_feedbackSamples = 1;
    m_feedbackFrame = 0;
    m_unlocked.clear();
}

void MeshCardScene::setFeedback(std::span<const McFeedback> elements, uint32_t samples)
{
    ++m_feedbackFrame;
    m_feedback.assign(elements.begin(), elements.end());
    m_feedbackSamples = std::max(samples, 1u);
}

void MeshCardScene::setUnlocked(uint32_t pageIndex, bool unlocked)
{
    PageEntry& page = m_pages[pageIndex];
    if (page.unlocked == unlocked) return;
    if (unlocked)
    {
        page.lastUsed = m_feedbackFrame;
        m_unlocked.emplace(page.lastUsed, pageIndex);
    }
    else
        m_unlocked.erase({ page.lastUsed, pageIndex });
    page.unlocked = unlocked;
}

void MeshCardScene::touchPage(uint32_t pageIndex)
{
    PageEntry& page = m_pages[pageIndex];
    if (!page.unlocked || page.lastUsed == m_feedbackFrame) return;
    m_unlocked.erase({ page.lastUsed, pageIndex });
    page.lastUsed = m_feedbackFrame;
    m_unlocked.emplace(page.lastUsed, pageIndex);
}

bool MeshCardScene::evictOldest(uint32_t maxFramesSinceLastUsed, std::vector<uint32_t>& dirtyCards)
{
    if (m_unlocked.empty()) return false;
    const std::pair<uint32_t, uint32_t> oldest = *m_unlocked.begin();
    if ((uint64_t)oldest.first + maxFramesSinceLastUsed > m_feedbackFrame) return false;
    const int32_t card = m_pages[oldest.second].card;
    setUnlocked(oldest.second, false);
    unmapPage(oldest.second);
    markDirty(m_dirty.pages, oldest.second);
    if (card >= 0) dirtyCards.push_back((uint32_t)card);
    ++m_stats.evictedPages;
    return true;
}

bool MeshCardScene::spanAvailable(uint32_t size) const
{
    if (m_pages.size() + size <= m_settings.maxPages) return true;
    for (const auto& span : m_freeSpans)
        if (span.second >= size) return true;
    return false;
}

void MeshCardScene::refreshInstance(uint32_t sceneInstance)
{
    if (!hasInstance(sceneInstance)) return;
    const MeshCardsEntry& e = m_meshCards[m_instanceMap[sceneInstance]];
    for (uint32_t c = 0; c < e.cardCount; ++c)
    {
        const Card& card = m_cards[e.firstCard + c];
        if (!card.allocated()) continue;
        for (uint32_t level = card.minAllocatedResLevel; level <= card.maxAllocatedResLevel; ++level)
        {
            const MipMap& m = mip(card, level);
            for (uint32_t local = 0; local < m.pageTableSize; ++local)
                if (m_pages[(uint32_t)m.pageTableOffset + local].mapped()) m_refreshQueue.push_back((uint32_t)m.pageTableOffset + local);
        }
    }
}

uint32_t MeshCardScene::addCardSpan(uint32_t size)
{
    for (auto it = m_freeCardSpans.begin(); it != m_freeCardSpans.end(); ++it)
        if (it->second >= size)
        {
            const uint32_t offset = it->first, rest = it->second - size;
            m_freeCardSpans.erase(it);
            if (rest != 0) m_freeCardSpans[offset + size] = rest;
            return offset;
        }
    const uint32_t offset = (uint32_t)m_cards.size();
    m_cards.resize(m_cards.size() + size);
    m_cardsGpu.resize(m_cards.size());
    return offset;
}

void MeshCardScene::removeCardSpan(uint32_t offset, uint32_t size)
{
    auto it = m_freeCardSpans.emplace(offset, size).first;
    const auto next = std::next(it);
    if (next != m_freeCardSpans.end() && it->first + it->second == next->first)
    {
        it->second += next->second;
        m_freeCardSpans.erase(next);
    }
    if (it != m_freeCardSpans.begin())
    {
        const auto prev = std::prev(it);
        if (prev->first + prev->second == it->first)
        {
            prev->second += it->second;
            m_freeCardSpans.erase(it);
        }
    }
}

void MeshCardScene::removeInstance(uint32_t sceneInstance)
{
    if (!hasInstance(sceneInstance)) return;
    const uint32_t index = m_instanceMap[sceneInstance];
    MeshCardsEntry& e = m_meshCards[index];
    for (uint32_t c = 0; c < e.cardCount; ++c)
    {
        const uint32_t cardIndex = e.firstCard + c;
        removeCardFromAtlas(cardIndex);
        m_cards[cardIndex] = Card{};
        m_cardsGpu[cardIndex] = McCardGpu{};
        markDirty(m_dirty.cards, cardIndex);
    }
    if (e.cardCount != 0) removeCardSpan(e.firstCard, e.cardCount);
    e = MeshCardsEntry{};
    m_meshCardsGpu[index] = McMeshCardsGpu{};
    markDirty(m_dirty.meshCards, index);
    m_freeMeshCards.push_back(index);
    m_instanceMap[sceneInstance] = mc::kNone;
    m_dirty.instanceMap = true;
}

uint32_t MeshCardScene::addInstance(uint32_t sceneInstance, const scene::MeshCards& cards, const float3x4& objectToWorld, uint32_t lightingChannels, bool emissiveLightSource)
{
    if (sceneInstance < m_instanceMap.size() && m_instanceMap[sceneInstance] != mc::kNone)
        throw Error("mesh cards: scene instance " + std::to_string(sceneInstance) + " already has cards");
    if (sceneInstance >= m_instanceMap.size())
    {
        m_instanceMap.resize((size_t)sceneInstance + 1, mc::kNone);
        m_dirty.instanceMap = true;
    }
    const float3 column0{ objectToWorld.m[0][0], objectToWorld.m[1][0], objectToWorld.m[2][0] };
    const float scale = length(column0);
    if (!(scale > 0) || cards.cards.empty()) return mc::kNone;

    const float3 size = (cards.boundsMax - cards.boundsMin) * scale;
    const float largestFace = std::max(size.y * size.z, std::max(size.x * size.z, size.x * size.y));
    const float minArea = m_settings.minSize * m_settings.minSize * (emissiveLightSource ? m_settings.emissiveMinAreaScale : 1.0f);
    if (!(largestFace > minArea)) return mc::kNone;

    // the cards that pass the size rule (MeshCardCullTest), at most 32
    std::vector<uint32_t> kept;
    for (uint32_t i = 0; i < cards.cards.size() && kept.size() < mc::kMaxCardsPerMesh; ++i)
    {
        const scene::MeshCard& c = cards.cards[i];
        if (4.0f * c.extent.x * c.extent.y * scale * scale > minArea) kept.push_back(i);
    }
    if (kept.empty()) return mc::kNone;

    uint32_t index;
    if (!m_freeMeshCards.empty())
    {
        index = m_freeMeshCards.back();
        m_freeMeshCards.pop_back();
    }
    else
    {
        index = (uint32_t)m_meshCards.size();
        m_meshCards.emplace_back();
        m_meshCardsGpu.emplace_back();
    }
    MeshCardsEntry entry;
    entry.sceneInstance = sceneInstance;
    entry.firstCard = addCardSpan((uint32_t)kept.size());
    entry.cardCount = (uint32_t)kept.size();
    entry.mostlyTwoSided = cards.mostlyTwoSided;
    entry.lightingChannels = lightingChannels & 7u;
    entry.emissiveLightSource = emissiveLightSource;
    m_meshCards[index] = entry;
    uint32_t slot = entry.firstCard;
    for (uint32_t i : kept)
    {
        const scene::MeshCard& c = cards.cards[i];
        Card card;
        card.live = true;
        card.meshCards = index;
        card.direction = (uint8_t)c.direction;
        card.origin = c.origin * scale;
        card.extent = c.extent * scale;
        float3 x, y, z;
        scene::meshCardAxes(c.direction, x, y, z);
        card.boxExtent = { std::fabs(x.x) * card.extent.x + std::fabs(y.x) * card.extent.y + std::fabs(z.x) * card.extent.z,
                           std::fabs(x.y) * card.extent.x + std::fabs(y.y) * card.extent.y + std::fabs(z.y) * card.extent.z,
                           std::fabs(x.z) * card.extent.x + std::fabs(y.z) * card.extent.y + std::fabs(z.z) * card.extent.z };
        // FLumenCard::ComputeAndSetResLevelXYBias: the shorter side loses levels
        const float aspect = card.extent.x / card.extent.y;
        uint32_t biasX = 0, biasY = 0;
        if (aspect >= 1.0f)
            biasY = floorLog2((uint32_t)std::max(1.0f, std::round(aspect)));
        else
            biasX = floorLog2((uint32_t)std::max(1.0f, std::round(1.0f / aspect)));
        card.biasX = (uint8_t)std::min(biasX, mc::kMaxResLevel - mc::kMinResLevel);
        card.biasY = (uint8_t)std::min(biasY, mc::kMaxResLevel - mc::kMinResLevel);
        m_cards[slot++] = card;
    }
    m_instanceMap[sceneInstance] = index;
    m_dirty.instanceMap = true;
    setTransform(sceneInstance, objectToWorld);
    return index;
}

void MeshCardScene::setTransform(uint32_t sceneInstance, const float3x4& objectToWorld)
{
    if (sceneInstance >= m_instanceMap.size() || m_instanceMap[sceneInstance] == mc::kNone) return;
    const uint32_t index = m_instanceMap[sceneInstance];
    MeshCardsEntry& e = m_meshCards[index];
    e.objectToWorld = objectToWorld;
    const float3 column0{ objectToWorld.m[0][0], objectToWorld.m[1][0], objectToWorld.m[2][0] };
    e.scale = length(column0);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) e.rotation[r][c] = objectToWorld.m[r][c] / e.scale;
    writeMeshCardsGpu(index);
    for (uint32_t c = 0; c < e.cardCount; ++c)
    {
        writeCardGpu(e.firstCard + c);
        markDirty(m_dirty.cards, e.firstCard + c);
    }
}

MeshCardScene::MipMapDesc MeshCardScene::mipMapDesc(const Card& card, uint32_t resLevel) const
{
    MipMapDesc d{};
    d.resLevelX = (uint32_t)std::clamp((int)resLevel - (int)card.biasX, (int)mc::kMinResLevel, (int)mc::kMaxResLevel);
    d.resLevelY = (uint32_t)std::clamp((int)resLevel - (int)card.biasY, (int)mc::kMinResLevel, (int)mc::kMaxResLevel);
    if (d.resLevelX > mc::kSubAllocResLevel || d.resLevelY > mc::kSubAllocResLevel)
    {
        // more than a physical page: whole pages, each showing a virtual page of the card
        d.resLevelX = std::max(d.resLevelX, mc::kSubAllocResLevel);
        d.resLevelY = std::max(d.resLevelY, mc::kSubAllocResLevel);
        d.subAllocation = false;
        d.sizeInPagesX = 1u << (d.resLevelX - mc::kSubAllocResLevel);
        d.sizeInPagesY = 1u << (d.resLevelY - mc::kSubAllocResLevel);
        d.resolutionX = d.sizeInPagesX * mc::kVirtualPage;
        d.resolutionY = d.sizeInPagesY * mc::kVirtualPage;
        d.pageResolutionX = d.pageResolutionY = mc::kPhysicalPage;
    }
    else
    {
        d.subAllocation = true;
        d.sizeInPagesX = d.sizeInPagesY = 1;
        d.resolutionX = d.pageResolutionX = 1u << d.resLevelX;
        d.resolutionY = d.pageResolutionY = 1u << d.resLevelY;
    }
    return d;
}

void MeshCardScene::updateMinMax(Card& card)
{
    card.minAllocatedResLevel = 0xFF;
    card.maxAllocatedResLevel = 0;
    for (uint32_t level = mc::kMinResLevel; level <= mc::kMaxResLevel; ++level)
        if (mip(card, level).allocated())
        {
            card.minAllocatedResLevel = (uint8_t)std::min<uint32_t>(card.minAllocatedResLevel, level);
            card.maxAllocatedResLevel = (uint8_t)std::max<uint32_t>(card.maxAllocatedResLevel, level);
        }
}

int32_t MeshCardScene::addSpan(uint32_t size)
{
    for (auto it = m_freeSpans.begin(); it != m_freeSpans.end(); ++it)
        if (it->second >= size)
        {
            const int32_t offset = it->first;
            const uint32_t rest = it->second - size;
            m_freeSpans.erase(it);
            if (rest != 0) m_freeSpans[offset + (int32_t)size] = rest;
            return offset;
        }
    const int32_t offset = (int32_t)m_pages.size();
    m_pages.resize(m_pages.size() + size);
    m_pagesGpu.resize(m_pages.size());
    m_pageTableGpu.resize(m_pages.size());
    return offset;
}

void MeshCardScene::removeSpan(int32_t offset, uint32_t size)
{
    auto it = m_freeSpans.emplace(offset, size).first;
    const auto next = std::next(it);
    if (next != m_freeSpans.end() && it->first + (int32_t)it->second == next->first)
    {
        it->second += next->second;
        m_freeSpans.erase(next);
    }
    if (it != m_freeSpans.begin())
    {
        const auto prev = std::prev(it);
        if (prev->first + (int32_t)prev->second == it->first)
        {
            prev->second += it->second;
            m_freeSpans.erase(it);
        }
    }
}

void MeshCardScene::reallocVirtualSurface(Card& card, uint32_t cardIndex, uint32_t resLevel, bool lock)
{
    MipMap& m = mip(card, resLevel);
    if (m.pageTableSize > 0)
    {
        // the level is there: its mapped pages follow the lock (a locked level's pages are not in the eviction order)
        if (m.locked != lock)
            for (uint32_t local = 0; local < m.pageTableSize; ++local)
            {
                const uint32_t pageIndex = (uint32_t)m.pageTableOffset + local;
                if (m_pages[pageIndex].mapped()) setUnlocked(pageIndex, !lock);
            }
        m.locked = lock;
        return;
    }
    const MipMapDesc d = mipMapDesc(card, resLevel);
    m.locked = lock;
    m.sizeInPagesX = (uint8_t)d.sizeInPagesX;
    m.sizeInPagesY = (uint8_t)d.sizeInPagesY;
    m.resLevelX = (uint8_t)d.resLevelX;
    m.resLevelY = (uint8_t)d.resLevelY;
    m.pageTableSize = (uint16_t)(d.sizeInPagesX * d.sizeInPagesY);
    m.pageTableOffset = addSpan(m.pageTableSize);
    for (uint32_t local = 0; local < m.pageTableSize; ++local)
    {
        const uint32_t pageIndex = (uint32_t)m.pageTableOffset + local;
        PageEntry& page = m_pages[pageIndex];
        page = PageEntry{};
        page.card = (int32_t)cardIndex;
        page.resLevel = (uint8_t)resLevel;
        page.subAllocX = d.subAllocation ? (int32_t)d.resolutionX : -1;
        page.subAllocY = d.subAllocation ? (int32_t)d.resolutionY : -1;
        const uint32_t px = local % d.sizeInPagesX, py = local / d.sizeInPagesX;
        float rect[4] = { (float)px / (float)d.sizeInPagesX, (float)py / (float)d.sizeInPagesY, (float)(px + 1) / (float)d.sizeInPagesX,
                          (float)(py + 1) / (float)d.sizeInPagesY };
        // every page has a 0.5 texel border for bilinear sampling, needed only on interior page edges
        const float border = 0.5f * (float)(mc::kPhysicalPage - mc::kVirtualPage);
        const float bx = border * (rect[2] - rect[0]) / (float)mc::kPhysicalPage, by = border * (rect[3] - rect[1]) / (float)mc::kPhysicalPage;
        if (px > 0) rect[0] -= bx;
        if (py > 0) rect[1] -= by;
        if (px < d.sizeInPagesX - 1) rect[2] += bx;
        if (py < d.sizeInPagesY - 1) rect[3] += by;
        std::memcpy(page.cardUvRect, rect, sizeof(rect));
        markDirty(m_dirty.pages, pageIndex);
    }
    updateMinMax(card);
    markDirty(m_dirty.cards, cardIndex);
}

void MeshCardScene::unmapPage(uint32_t pageIndex)
{
    PageEntry& page = m_pages[pageIndex];
    if (!page.mapped()) return;
    setUnlocked(pageIndex, false);
    m_allocator.free(page);
    page.pageCoordX = page.pageCoordY = -1;
    page.sampleAtlasBiasX = page.sampleAtlasBiasY = 0;
    page.sampleResLevelX = page.sampleResLevelY = 0;
}

void MeshCardScene::freeVirtualSurface(Card& card, uint32_t fromLevel, uint32_t toLevel)
{
    if (!card.allocated()) return;
    for (uint32_t level = fromLevel; level <= toLevel && level <= mc::kMaxResLevel; ++level)
    {
        if (level < mc::kMinResLevel) continue;
        MipMap& m = mip(card, level);
        if (!m.allocated()) continue;
        for (uint32_t local = 0; local < m.pageTableSize; ++local)
        {
            const uint32_t pageIndex = (uint32_t)m.pageTableOffset + local;
            unmapPage(pageIndex);
            m_pages[pageIndex] = PageEntry{};
            markDirty(m_dirty.pages, pageIndex);
        }
        removeSpan(m.pageTableOffset, m.pageTableSize);
        m = MipMap{};
    }
    updateMinMax(card);
}

void MeshCardScene::mapPage(const MipMap& mipMap, uint32_t pageIndex)
{
    PageEntry& page = m_pages[pageIndex];
    if (page.mapped()) return;
    m_allocator.allocate(page);
    if (page.mapped())
    {
        page.samplePage = pageIndex;
        page.sampleAtlasBiasX = page.rect[0] / (1u << mc::kMinResLevel);
        page.sampleAtlasBiasY = page.rect[1] / (1u << mc::kMinResLevel);
        page.sampleResLevelX = mipMap.resLevelX;
        page.sampleResLevelY = mipMap.resLevelY;
        if (!mipMap.locked) setUnlocked(pageIndex, true);
    }
    markDirty(m_dirty.pages, pageIndex);
}

// Removes levels without a mapped page and points every unmapped page of a level at the nearest coarser mapped page.
void MeshCardScene::updateMipHierarchy(Card& card)
{
    if (!card.allocated()) return;
    for (uint32_t level = card.minAllocatedResLevel; level <= card.maxAllocatedResLevel; ++level)
    {
        MipMap& m = mip(card, level);
        if (!m.allocated()) continue;
        bool any = false;
        for (uint32_t local = 0; local < m.pageTableSize && !any; ++local) any = m_pages[(uint32_t)m.pageTableOffset + local].mapped();
        if (!any) freeVirtualSurface(card, level, level);
    }
    updateMinMax(card);
    if (!card.allocated()) return;
    uint32_t parentLevel = card.minAllocatedResLevel;
    for (uint32_t level = parentLevel + 1; level <= card.maxAllocatedResLevel; ++level)
    {
        MipMap& m = mip(card, level);
        if (m.pageTableSize == 0) continue;
        const MipMap& parent = mip(card, parentLevel);
        for (uint32_t local = 0; local < m.pageTableSize; ++local)
        {
            const uint32_t pageIndex = (uint32_t)m.pageTableOffset + local;
            PageEntry& page = m_pages[pageIndex];
            if (page.mapped()) continue;
            const uint32_t px = local % m.sizeInPagesX, py = local / m.sizeInPagesX;
            const uint32_t ppx = px * parent.sizeInPagesX / m.sizeInPagesX, ppy = py * parent.sizeInPagesY / m.sizeInPagesY;
            const PageEntry& up = m_pages[(uint32_t)parent.pageTableOffset + ppx + ppy * parent.sizeInPagesX];
            page.samplePage = up.samplePage;
            page.sampleAtlasBiasX = up.sampleAtlasBiasX;
            page.sampleAtlasBiasY = up.sampleAtlasBiasY;
            page.sampleResLevelX = up.sampleResLevelX;
            page.sampleResLevelY = up.sampleResLevelY;
            markDirty(m_dirty.pages, pageIndex);
        }
        parentLevel = level;
    }
}

void MeshCardScene::removeCardFromAtlas(uint32_t cardIndex)
{
    Card& card = m_cards[cardIndex];
    card.desiredLockedResLevel = 0;
    card.desiredLockedResLevelOnLastAlloc = 0;
    if (card.allocated()) freeVirtualSurface(card, card.minAllocatedResLevel, card.maxAllocatedResLevel);
    markDirty(m_dirty.cards, cardIndex);
}

void MeshCardScene::captureOf(uint32_t pageIndex, const PageEntry& page, const PageEntry& capture, uint32_t cardIndex, bool resample, bool refresh)
{
    McCapture cap;
    cap.page = pageIndex;
    cap.card = cardIndex;
    cap.sceneInstance = m_meshCards[m_cards[cardIndex].meshCards].sceneInstance;
    cap.captureRect[0] = capture.rect[0];
    cap.captureRect[1] = capture.rect[1];
    cap.captureRect[2] = capture.rect[2] - capture.rect[0];
    cap.captureRect[3] = capture.rect[3] - capture.rect[1];
    cap.atlasRect[0] = page.rect[0];
    cap.atlasRect[1] = page.rect[1];
    cap.atlasRect[2] = page.rect[2] - page.rect[0];
    cap.atlasRect[3] = page.rect[3] - page.rect[1];
    std::memcpy(cap.cardUvRect, page.cardUvRect, sizeof(cap.cardUvRect));
    cap.resample = resample;
    cap.refresh = refresh;
    m_captures.push_back(cap);
    m_stats.capturedTexels += cap.atlasRect[2] * cap.atlasRect[3];
}

void MeshCardScene::update(std::span<const float3> viewOrigins)
{
    ++m_frame;
    m_captures.clear();
    m_requests.clear();
    m_stats.requests = m_stats.captures = m_stats.capturedTexels = m_stats.loweredAllocations = 0;
    m_stats.desiredTexels = 0;
    m_stats.feedbackElements = m_stats.hiResRequests = m_stats.hiResMapped = m_stats.evictedPages = 0;
    uint32_t histogram[mc::kDistanceBuckets] = {};
    std::vector<uint32_t> hide;

    // FLumenSurfaceCacheUpdateMeshCardsTask: every card's distance and the level it asks for
    for (uint32_t mcIndex = 0; mcIndex < m_meshCards.size(); ++mcIndex)
    {
        const MeshCardsEntry& e = m_meshCards[mcIndex];
        if (e.cardCount == 0) continue;  // (a removed entry)
        const float3 translation{ e.objectToWorld.m[0][3], e.objectToWorld.m[1][3], e.objectToWorld.m[2][3] };
        float3 local[8];
        const size_t views = std::min<size_t>(viewOrigins.size(), 8);
        for (size_t v = 0; v < views; ++v)
        {
            const float3 r = viewOrigins[v] - translation;
            local[v] = { e.rotation[0][0] * r.x + e.rotation[1][0] * r.y + e.rotation[2][0] * r.z,
                         e.rotation[0][1] * r.x + e.rotation[1][1] * r.y + e.rotation[2][1] * r.z,
                         e.rotation[0][2] * r.x + e.rotation[1][2] * r.y + e.rotation[2][2] * r.z };
        }
        for (uint32_t c = 0; c < e.cardCount; ++c)
        {
            const uint32_t cardIndex = e.firstCard + c;
            Card& card = m_cards[cardIndex];
            float distance = 3.4e38f;
            for (size_t v = 0; v < views; ++v)
            {
                float sq = 0;
                for (int a = 0; a < 3; ++a)
                {
                    const float d = std::max(0.0f, std::fabs(comp(local[v], a) - comp(card.origin, a)) - comp(card.boxExtent, a));
                    sq += d * d;
                }
                distance = std::min(distance, std::max(std::sqrt(sq), 1.0f));
            }
            card.distance = distance;
            const float maxExtent = std::max(card.extent.x, card.extent.y);
            const float projected = std::min(m_settings.texelDensityScale * maxExtent / distance, m_settings.maxTexelDensity * maxExtent);
            const uint32_t snapped = roundUpPow2(std::min((uint32_t)std::max(projected, 0.0f), m_settings.maxResolution));
            // (an emissive light source: down to a resolution of 1 - the reference's MinCardResolution for it)
            const bool visible = distance < m_settings.maxDistance && snapped >= (e.emissiveLightSource ? 1u : m_settings.minResolution);
            const uint32_t resLevel = floorLog2(std::max(snapped, 1u << mc::kMinResLevel));
            if (!visible)
            {
                if (card.visible) hide.push_back(cardIndex);
                continue;
            }
            card.desiredLockedResLevel = (uint8_t)resLevel;
            const MipMapDesc want = mipMapDesc(card, resLevel);
            m_stats.desiredTexels += (uint64_t)want.sizeInPagesX * want.pageResolutionX * want.sizeInPagesY * want.pageResolutionY;
            if (resLevel == card.desiredLockedResLevelOnLastAlloc) continue;
            float d = distance;
            if (card.visible)
            {
                // a reallocation matters less than a new card
                const float delta = std::fabs((float)card.desiredLockedResLevelOnLastAlloc - (float)resLevel);
                d += (1.0f - std::clamp((delta + 1.0f) / 3.0f, 0.0f, 1.0f)) * 25.0f;
            }
            Request r;
            r.card = cardIndex;
            r.resLevel = (uint8_t)resLevel;
            r.distanceBin = (uint8_t)distanceBin(d);
            ++histogram[r.distanceBin];
            m_requests.push_back(r);
        }
    }
    m_stats.requests = (uint32_t)m_requests.size();

    for (uint32_t cardIndex : hide)
    {
        m_cards[cardIndex].visible = false;
        removeCardFromAtlas(cardIndex);
    }

    // UpdateSurfaceCacheFeedback: the pages the newest completed frame's hits asked for. A mapped one counts as used in
    // this feedback frame; one that is not mapped is requested - less urgent than the resident levels (25 m further),
    // and the more so the smaller its share of the frame's hits (up to 25 m more).
    for (const McFeedback& f : m_feedback)
    {
        if (f.hits <= m_settings.feedbackMinPageHits || f.card >= m_cards.size()) continue;
        Card& card = m_cards[f.card];
        if (!card.live || !card.visible || !card.allocated()) continue;
        ++m_stats.feedbackElements;
        const uint32_t level = std::clamp<uint32_t>(f.resLevel, mc::kMinResLevel, mc::kMaxResLevel);
        if (level <= card.minAllocatedResLevel) continue;  // (the resident level holds it)
        const MipMapDesc want = mipMapDesc(card, level);
        const uint32_t local = std::min<uint32_t>(f.pageX, want.sizeInPagesX - 1) + std::min<uint32_t>(f.pageY, want.sizeInPagesY - 1) * want.sizeInPagesX;
        const MipMap& m = mip(card, level);
        if (m.allocated() && m_pages[(uint32_t)m.pageTableOffset + local].mapped())
        {
            if (!m.locked) touchPage((uint32_t)m.pageTableOffset + local);
            continue;
        }
        const float share = std::min((float)f.hits / (float)m_feedbackSamples, 1.0f);
        Request r;
        r.card = f.card;
        r.resLevel = (uint8_t)level;
        r.localPage = (uint16_t)local;
        r.distanceBin = (uint8_t)distanceBin(card.distance + 25.0f + 25.0f * (1.0f - share));
        ++histogram[r.distanceBin];
        m_requests.push_back(r);
        ++m_stats.hiResRequests;
    }
    m_feedback.clear();
    m_stats.requests = (uint32_t)m_requests.size();  // (with the feedback's)

    // the frame's requests: the nearest buckets, up to the capture count
    std::vector<Request> chosen;
    {
        uint32_t count = 0, lastBucket = 0, lastBucketCount = 0;
        for (; lastBucket < mc::kDistanceBuckets; ++lastBucket)
        {
            count += histogram[lastBucket];
            if (count >= m_settings.capturesPerFrame)
            {
                lastBucketCount = m_settings.capturesPerFrame - (count - histogram[lastBucket]);
                count = m_settings.capturesPerFrame;
                break;
            }
        }
        if (lastBucket == mc::kDistanceBuckets) lastBucketCount = 0xFFFFFFFFu;  // every bucket fits
        for (const Request& r : m_requests)
        {
            if (count == 0) break;
            if (r.distanceBin > lastBucket) continue;
            if (r.distanceBin == lastBucket)
            {
                if (lastBucketCount == 0) continue;
                --lastBucketCount;
            }
            chosen.push_back(r);
            --count;
        }
    }

    // ProcessLumenSurfaceCacheRequests: the locked levels first, the feedback's pages after them
    Allocator captureAllocator;
    captureAllocator.init(m_captureAtlasPages);
    std::vector<uint32_t> dirtyCards;
    std::vector<Request> hiRes;
    uint32_t lockedDone = 0;
    for (const Request& request : chosen)
    {
        Card& card = m_cards[request.card];
        if (request.localPage != kLockedMip)
        {
            if (card.live && card.visible && card.allocated() && request.resLevel > card.minAllocatedResLevel) hiRes.push_back(request);
            if (m_captures.size() + hiRes.size() >= m_settings.capturesPerFrame) break;
            continue;
        }
        uint32_t level = request.resLevel;
        bool canAlloc = m_allocator.spaceAvailable(mipMapDesc(card, level), false);
        // the atlas is full: the feedback pages no hit asked for in the last 2 feedback frames make room (one page an
        // iteration: at most as many as there are unlocked pages)
        while (!canAlloc && evictOldest(2, dirtyCards)) canAlloc = m_allocator.spaceAvailable(mipMapDesc(card, level), false);
        // ... and then a lower level
        while (!canAlloc && level > mc::kMinResLevel)
        {
            --level;
            canAlloc = m_allocator.spaceAvailable(mipMapDesc(card, level), false);
        }
        if (canAlloc && level != request.resLevel) ++m_stats.loweredAllocations;
        if (!captureAllocator.spaceAvailable(mipMapDesc(card, level), false)) canAlloc = false;
        {
            // (the page table is a fixed-size GPU buffer: a card that would not fit waits)
            const MipMapDesc want = mipMapDesc(card, level);
            if (!mip(card, level).allocated() && !spanAvailable(want.sizeInPagesX * want.sizeInPagesY)) canAlloc = false;
        }
        if (canAlloc)
        {
            card.visible = true;
            card.desiredLockedResLevelOnLastAlloc = request.resLevel;
            const bool resample = card.allocated();
            if (card.allocated())
            {
                freeVirtualSurface(card, card.minAllocatedResLevel, card.minAllocatedResLevel);
                if (card.allocated() && level > mc::kMinResLevel) freeVirtualSurface(card, card.minAllocatedResLevel, level - 1);
            }
            reallocVirtualSurface(card, request.card, level, true);
            const MipMap& m = mip(card, card.minAllocatedResLevel);
            for (uint32_t local = 0; local < m.pageTableSize; ++local)
            {
                const uint32_t pageIndex = (uint32_t)m.pageTableOffset + local;
                PageEntry& page = m_pages[pageIndex];
                if (page.mapped()) continue;
                mapPage(m, pageIndex);
                if (!page.mapped()) throw Error("mesh cards: a page could not be mapped after the space check");
                PageEntry capture = page;  // the same size and kind in the capture atlas
                capture.pageCoordX = capture.pageCoordY = -1;
                captureAllocator.allocate(capture);
                if (!capture.mapped()) throw Error("mesh cards: the capture atlas is full after the space check");
                page.capturedFrame = m_frame;
                captureOf(pageIndex, page, capture, request.card, resample, false);
            }
            dirtyCards.push_back(request.card);
            ++lockedDone;
        }
        if (m_captures.size() + hiRes.size() >= m_settings.capturesPerFrame) break;
    }
    // (the locked levels' requests: what a level load waits for)
    const uint32_t lockedRequests = m_stats.requests - m_stats.hiResRequests;
    m_stats.pending = lockedRequests > lockedDone ? lockedRequests - lockedDone : 0;

    // The feedback's pages: one page of a level above the card's locked one, not locked. Room is made from the pages
    // used longest ago, but not from one a hit may still report: the feedback's tile jitter comes round in tile^2 frames.
    for (const Request& request : hiRes)
    {
        Card& card = m_cards[request.card];
        if (!card.allocated() || request.resLevel <= card.minAllocatedResLevel) continue;  // (its locked level rose this frame)
        const MipMapDesc want = mipMapDesc(card, request.resLevel);
        if (request.localPage >= want.sizeInPagesX * want.sizeInPagesY) continue;
        bool canAlloc = m_allocator.spaceAvailable(want, true);
        const uint32_t keep = m_settings.feedbackTileSize * m_settings.feedbackTileSize;
        while (!canAlloc && evictOldest(keep, dirtyCards)) canAlloc = m_allocator.spaceAvailable(want, true);
        if (!captureAllocator.spaceAvailable(want, true)) canAlloc = false;
        if (!mip(card, request.resLevel).allocated() && !spanAvailable(want.sizeInPagesX * want.sizeInPagesY)) canAlloc = false;
        if (!canAlloc) continue;
        reallocVirtualSurface(card, request.card, request.resLevel, false);
        const MipMap& m = mip(card, request.resLevel);
        const uint32_t pageIndex = (uint32_t)m.pageTableOffset + request.localPage;
        PageEntry& page = m_pages[pageIndex];
        if (page.mapped()) continue;
        mapPage(m, pageIndex);
        if (!page.mapped()) throw Error("mesh cards: a feedback page could not be mapped after the space check");
        PageEntry capture = page;
        capture.pageCoordX = capture.pageCoordY = -1;
        capture.unlocked = false;
        captureAllocator.allocate(capture);
        if (!capture.mapped()) throw Error("mesh cards: the capture atlas is full after the space check");
        page.capturedFrame = m_frame;
        captureOf(pageIndex, page, capture, request.card, true, false);  // (the card's lighting at the new resolution)
        dirtyCards.push_back(request.card);
        ++m_stats.hiResMapped;
        if (m_captures.size() >= m_settings.capturesPerFrame) break;
    }

    // Resident pages captured again (LastCapturedPageHeap, CardCaptureRefreshFraction): what the new cards left of the
    // frame's share, the pages in table order from where the last frame stopped - every resident page comes round.
    m_stats.refreshed = 0;
    if (m_settings.refreshFraction > 0 && !m_pages.empty())
    {
        const uint32_t refreshPages = (uint32_t)((float)m_settings.capturesPerFrame * m_settings.refreshFraction);
        const uint64_t refreshTexels =
            (uint64_t)((double)m_settings.atlasSize * m_settings.atlasSize / std::max(m_settings.captureFactor, 1u) * m_settings.refreshFraction);
        uint64_t texels = 0;
        const uint32_t count = (uint32_t)m_pages.size();
        // 0: the page is not to be captured (skip it), 1: captured, 2: the frame's share or the capture atlas is used up
        auto refresh = [&](uint32_t pageIndex) -> int {
            if (pageIndex >= count) return 0;
            PageEntry& page = m_pages[pageIndex];
            if (!page.mapped() || page.card < 0 || page.capturedFrame == m_frame) return 0;
            const uint64_t size = (uint64_t)(page.rect[2] - page.rect[0]) * (page.rect[3] - page.rect[1]);
            MipMapDesc d{};
            d.subAllocation = page.subAllocation();
            d.sizeInPagesX = d.sizeInPagesY = 1;
            d.resolutionX = page.rect[2] - page.rect[0];
            d.resolutionY = page.rect[3] - page.rect[1];
            if ((texels + size > refreshTexels && m_stats.refreshed > 0) || !captureAllocator.spaceAvailable(d, true)) return 2;
            PageEntry capture = page;
            capture.pageCoordX = capture.pageCoordY = -1;
            captureAllocator.allocate(capture);
            if (!capture.mapped()) return 2;
            page.capturedFrame = m_frame;
            captureOf(pageIndex, page, capture, (uint32_t)page.card, false, true);
            texels += size;
            ++m_stats.refreshed;
            return 1;
        };
        bool room = true;
        while (room && !m_refreshQueue.empty() && m_stats.refreshed < refreshPages && m_captures.size() < m_settings.capturesPerFrame)
        {
            if (refresh(m_refreshQueue.back()) == 2) room = false;
            else m_refreshQueue.pop_back();
        }
        const uint32_t start = m_refreshCursor;
        for (uint32_t step = 0; room && step < count && m_stats.refreshed < refreshPages && m_captures.size() < m_settings.capturesPerFrame; ++step)
        {
            const uint32_t pageIndex = (start + step) % count;
            const int result = refresh(pageIndex);
            if (result == 2)
            {
                m_refreshCursor = pageIndex;
                break;
            }
            if (result == 1) m_refreshCursor = (pageIndex + 1) % count;
        }
    }
    m_stats.captures = (uint32_t)m_captures.size();

    // feedback pages no hit asked for in a long while leave (the levels left without a page go with them, below)
    while (evictOldest(m_settings.keepUnusedPagesFrames, dirtyCards)) {}
    m_stats.hiResPages = (uint32_t)m_unlocked.size();

    sortUnique(dirtyCards);
    for (uint32_t cardIndex : dirtyCards)
    {
        updateMipHierarchy(m_cards[cardIndex]);
        markDirty(m_dirty.cards, cardIndex);
    }

    // GPU records of what changed
    sortUnique(m_dirty.cards);
    sortUnique(m_dirty.pages);
    for (uint32_t cardIndex : m_dirty.cards) writeCardGpu(cardIndex);
    for (uint32_t pageIndex : m_dirty.pages) writePageGpu(pageIndex);

    m_stats.meshCards = (uint32_t)m_meshCards.size();
    m_stats.cards = (uint32_t)m_cards.size();
    m_stats.visibleCards = 0;
    m_stats.allocatedTexels = 0;
    for (const Card& card : m_cards) m_stats.visibleCards += card.visible ? 1u : 0u;
    m_stats.mappedPages = 0;
    for (const PageEntry& page : m_pages)
        if (page.mapped())
        {
            ++m_stats.mappedPages;
            m_stats.allocatedTexels += (uint64_t)(page.rect[2] - page.rect[0]) * (page.rect[3] - page.rect[1]);
        }
    m_stats.freePhysicalPages = m_allocator.freePages();
}

void MeshCardScene::writeMeshCardsGpu(uint32_t index)
{
    const MeshCardsEntry& e = m_meshCards[index];
    McMeshCardsGpu& g = m_meshCardsGpu[index];
    g = McMeshCardsGpu{};
    // world -> mesh cards space: the rotation's transpose; w = the world origin
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c) g.worldToLocal[r][c] = e.rotation[c][r];
        g.worldToLocal[r][3] = e.objectToWorld.m[r][3];
    }
    g.cardOffset = e.firstCard;
    g.countFlags = e.cardCount | (e.mostlyTwoSided ? 1u << 17 : 0u) | ((e.lightingChannels ^ 1u) << 20);
    for (uint32_t& l : g.cardLookup) l = 0;
    for (uint32_t c = 0; c < e.cardCount; ++c) g.cardLookup[m_cards[e.firstCard + c].direction] |= 1u << c;
    markDirty(m_dirty.meshCards, index);
}

void MeshCardScene::writeCardGpu(uint32_t index)
{
    const Card& card = m_cards[index];
    const MeshCardsEntry& e = m_meshCards[card.meshCards];
    McCardGpu& g = m_cardsGpu[index];
    g = McCardGpu{};
    g.origin[0] = card.origin.x;
    g.origin[1] = card.origin.y;
    g.origin[2] = card.origin.z;
    g.extent[0] = card.extent.x;
    g.extent[1] = card.extent.y;
    g.extent[2] = card.extent.z;
    g.packed = (uint32_t)card.direction | (uint32_t)card.biasX << 4 | (uint32_t)card.biasY << 8 | (card.visible && card.allocated() ? 1u << 16 : 0u);
    g.meshCards = card.meshCards;
    if (card.allocated())
    {
        const MipMap& lo = mip(card, card.minAllocatedResLevel);
        const MipMap& hi = mip(card, card.maxAllocatedResLevel);
        g.sizeInPages = (uint32_t)lo.sizeInPagesX | (uint32_t)lo.sizeInPagesY << 16;
        g.pageTableOffset = (uint32_t)lo.pageTableOffset;
        g.hiResSizeInPages = (uint32_t)hi.sizeInPagesX | (uint32_t)hi.sizeInPagesY << 16;
        g.hiResPageTableOffset = (uint32_t)hi.pageTableOffset;
        const MipMapDesc d = mipMapDesc(card, card.minAllocatedResLevel);
        g.texelSize = 0.5f * (2.0f * card.extent.x / (float)d.resolutionX + 2.0f * card.extent.y / (float)d.resolutionY);
    }
    // card -> world: the card's axes (mesh axes) rotated to the world; w = the card centre in the world
    float3 axis[3];
    scene::meshCardAxes(card.direction, axis[0], axis[1], axis[2]);
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            g.cardToWorld[r][c] = e.rotation[r][0] * axis[c].x + e.rotation[r][1] * axis[c].y + e.rotation[r][2] * axis[c].z;
        g.cardToWorld[r][3] = e.rotation[r][0] * card.origin.x + e.rotation[r][1] * card.origin.y + e.rotation[r][2] * card.origin.z + e.objectToWorld.m[r][3];
    }
}

void MeshCardScene::writePageGpu(uint32_t index)
{
    const PageEntry& page = m_pages[index];
    McCardPageGpu& g = m_pagesGpu[index];
    McPageTableGpu& t = m_pageTableGpu[index];
    g = McCardPageGpu{};
    t = McPageTableGpu{};
    if (page.card < 0) return;
    t.packed = (page.sampleAtlasBiasX & 0xFFFu) | (page.sampleAtlasBiasY & 0xFFFu) << 12 | (page.sampleResLevelX & 0xFu) << 24 | (page.sampleResLevelY & 0xFu) << 28;
    t.cardPage = page.samplePage;
    if (!page.mapped()) return;
    const Card& card = m_cards[(uint32_t)page.card];
    const MipMap& m = mip(card, page.resLevel);
    g.card = (uint32_t)page.card;
    g.resLevelPageTableOffset = (uint32_t)m.pageTableOffset;
    g.sizeInTexels[0] = (float)(page.rect[2] - page.rect[0]);
    g.sizeInTexels[1] = (float)(page.rect[3] - page.rect[1]);
    std::memcpy(g.cardUvRect, page.cardUvRect, sizeof(g.cardUvRect));
    for (int i = 0; i < 4; ++i) g.atlasRect[i] = (float)page.rect[i];
    g.cardUvTexelScale[0] = (page.cardUvRect[2] - page.cardUvRect[0]) / g.sizeInTexels[0];
    g.cardUvTexelScale[1] = (page.cardUvRect[3] - page.cardUvRect[1]) / g.sizeInTexels[1];
    uint32_t tilesX = m.sizeInPagesX * (mc::kPhysicalPage / mc::kTile), tilesY = m.sizeInPagesY * (mc::kPhysicalPage / mc::kTile);
    if (page.subAllocation())
    {
        tilesX = (uint32_t)page.subAllocX / mc::kTile;
        tilesY = (uint32_t)page.subAllocY / mc::kTile;
    }
    g.resLevelSizeInTiles = tilesX | tilesY << 16;
}

MeshCardScene::Dirty MeshCardScene::takeDirty()
{
    sortUnique(m_dirty.meshCards);
    sortUnique(m_dirty.cards);
    sortUnique(m_dirty.pages);
    Dirty out = std::move(m_dirty);
    m_dirty = {};
    return out;
}

void MeshCardScene::validate() const
{
    const uint32_t texels = m_settings.atlasSize;
    std::vector<uint8_t> used((size_t)(texels / 8) * (texels / 8), 0);  // 8 x 8 blocks: the smallest allocation
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        const PageEntry& p = m_pages[i];
        if (!p.mapped()) continue;
        if (p.card < 0 || (size_t)p.card >= m_cards.size()) throw Error("mesh cards: page " + std::to_string(i) + " is mapped without a card");
        if (p.rect[2] > texels || p.rect[3] > texels || p.rect[0] >= p.rect[2] || p.rect[1] >= p.rect[3] || p.rect[0] % 8 != 0 || p.rect[1] % 8 != 0)
            throw Error("mesh cards: page " + std::to_string(i) + " has a bad atlas rectangle");
        for (uint32_t y = p.rect[1] / 8; y < p.rect[3] / 8; ++y)
            for (uint32_t x = p.rect[0] / 8; x < p.rect[2] / 8; ++x)
            {
                uint8_t& u = used[(size_t)y * (texels / 8) + x];
                if (u != 0) throw Error("mesh cards: page " + std::to_string(i) + " overlaps another page in the atlas");
                u = 1;
            }
        const Card& card = m_cards[(size_t)p.card];
        const MipMap& m = mip(card, p.resLevel);
        if (!m.allocated() || (int32_t)i < m.pageTableOffset || (int32_t)i >= m.pageTableOffset + (int32_t)m.pageTableSize)
            throw Error("mesh cards: page " + std::to_string(i) + " is outside its card's span");
    }
    for (size_t c = 0; c < m_cards.size(); ++c)
    {
        const Card& card = m_cards[c];
        if (card.visible && !card.allocated()) throw Error("mesh cards: card " + std::to_string(c) + " is visible without pages");
        if (!card.allocated()) continue;
        const MipMap& m = mip(card, card.minAllocatedResLevel);
        for (uint32_t local = 0; local < m.pageTableSize; ++local)
            if (!m_pages[(size_t)m.pageTableOffset + local].mapped())
                throw Error("mesh cards: card " + std::to_string(c) + " has an unmapped page in its locked level");
    }
    for (const McCapture& a : m_captures)
        if (a.captureRect[0] + a.captureRect[2] > captureAtlasSize() || a.captureRect[1] + a.captureRect[3] > captureAtlasSize())
            throw Error("mesh cards: a capture lies outside the capture atlas");
    // the eviction order holds exactly the mapped pages of unlocked levels
    size_t unlocked = 0;
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        const PageEntry& p = m_pages[i];
        if (!p.mapped())
        {
            if (p.unlocked) throw Error("mesh cards: page " + std::to_string(i) + " is in the eviction order without being mapped");
            continue;
        }
        const bool locked = mip(m_cards[(size_t)p.card], p.resLevel).locked;
        if (p.unlocked == locked) throw Error("mesh cards: page " + std::to_string(i) + " disagrees with its level's lock");
        if (p.unlocked)
        {
            ++unlocked;
            if (m_unlocked.count({ p.lastUsed, (uint32_t)i }) == 0) throw Error("mesh cards: page " + std::to_string(i) + " is missing from the eviction order");
        }
    }
    if (unlocked != m_unlocked.size()) throw Error("mesh cards: the eviction order holds pages that are not unlocked");
}
} // namespace unx::render::refl
