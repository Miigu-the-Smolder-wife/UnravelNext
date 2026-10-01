// surface_cache.mesh_cards_test_set (unx/refl/CardSet.h): a hand-made mesh card set in the agreed layout
// (Docs/Status/MESH_CARDS_INTERFACE_KO.md sections 2-3), built on the CPU from the scene's source data.
#include "unx/refl/CardSet.h"

#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace unx::render::refl
{
namespace
{
constexpr uint32_t kAtlas = 4096, kPage = 128, kMinSide = 8;
constexpr float kTexelsPerMetre = 20.0f, kNormalThreshold = 0.25f, kBoundsPad = 0.01f;

struct MeshCardsRecord  // 80 B (McMeshCards)
{
    float worldToLocal[3][4];
    uint32_t cardOffset, countFlags;
    uint32_t cardLookup[6];
};
struct CardRecord  // 112 B (McCard)
{
    float origin[3];
    uint32_t packed;
    float extent[3];
    float texelSize;
    uint32_t sizeInPages, pageTableOffset, hiResSizeInPages, hiResPageTableOffset;
    float cardToWorld[3][4];
    uint32_t meshCards, pad[3];
};
struct CardPageRecord  // 64 B (McCardPage)
{
    uint32_t card, resLevelPageTableOffset;
    float sizeInTexels[2];
    float cardUvRect[4];
    float atlasRect[4];
    float cardUvTexelScale[2];
    uint32_t resLevelSizeInTiles, pad;
};
static_assert(sizeof(MeshCardsRecord) == 80 && sizeof(CardRecord) == 112 && sizeof(CardPageRecord) == 64);

uint32_t pow2Ceil(float v)
{
    uint32_t r = 1;
    while ((float)r < v) r <<= 1;
    return r;
}
uint32_t log2Of(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v) ++r;
    return r;
}
// R11G11B10_FLOAT from non-negative floats.
uint32_t packFloat(float v, int mantissaBits)
{
    if (!(v > 0)) return 0;
    const float top = mantissaBits == 6 ? 65024.0f : 64512.0f;
    v = std::min(v, top);
    int e;
    const float m = std::frexp(v, &e);  // v = m x 2^e, m in [0.5, 1)
    int exponent = e - 1 + 15;
    const int scale = 1 << mantissaBits;
    if (exponent <= 0) return (uint32_t)std::min((float)(scale - 1), std::round(v / std::ldexp(1.0f, -14) * scale));  // denormal
    int mantissa = (int)std::round((m * 2 - 1) * scale);
    if (mantissa == scale)
    {
        mantissa = 0;
        ++exponent;
    }
    return ((uint32_t)exponent << mantissaBits) | (uint32_t)mantissa;
}
uint32_t packR11G11B10(float3 c) { return packFloat(c.x, 6) | (packFloat(c.y, 6) << 11) | (packFloat(c.z, 5) << 22); }

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 16);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "card test set buffer");
    r->SetName(name);
    return r;
}
ComPtr<ID3D12Resource> makeTexture(Device& device, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = kAtlas;
    d.Height = kAtlas;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;  // (imported like the engine's other persistent textures)
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "card test set atlas");
    r->SetName(name);
    return r;
}
} // namespace

struct CardTestSet::Impl
{
    Device& device;
    uint32_t revision = UINT32_MAX;
    uint64_t generation = 0;
    ComPtr<ID3D12Resource> buffers[5];  // instance map, mesh cards, cards, card pages, page table
    uint64_t bufferBytes[5] = {};
    ComPtr<ID3D12Resource> textures[4];  // depth, albedo, normal, emissive
    ComPtr<ID3D12Resource> staging;
    uint64_t bufferOffset[5] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[4] = {};
    bool pending = false;
    uint32_t pages = 0, instances = 0;

    explicit Impl(Device& d) : device(d) {}
    void build(const GpuScene& gpuScene);
};

CardTestSet::CardTestSet(Device& device) : m(std::make_unique<Impl>(device)) {}
CardTestSet::~CardTestSet() = default;

void CardTestSet::Impl::build(const GpuScene& gpuScene)
{
    const scene::Scene* source = gpuScene.source();
    if (!source) fail("surface_cache.mesh_cards_test_set: the scene has no CPU source data");
    const scene::Scene& sc = *source;
    std::vector<uint16_t> depth((size_t)kAtlas * kAtlas, 0xFFFF);
    std::vector<uint32_t> albedo((size_t)kAtlas * kAtlas, 0), emissive((size_t)kAtlas * kAtlas, 0);
    std::vector<uint16_t> normal((size_t)kAtlas * kAtlas, 0x8080);
    std::vector<uint32_t> instanceMap(sc.instances.size(), 0xFFFFFFFFu);
    std::vector<MeshCardsRecord> meshCards;
    std::vector<CardRecord> cards;
    std::vector<CardPageRecord> cardPages;
    std::vector<uint32_t> pageTable;  // uint2 per entry
    // shelf packing of 8-aligned rectangles
    uint32_t shelfX = 0, shelfY = 0, shelfHeight = 0;
    bool full = false;
    uint32_t skipped = 0;
    std::vector<float> cardDepth;
    std::vector<uint32_t> cardAlbedo, cardEmissive;
    std::vector<uint16_t> cardNormal;

    for (size_t i = 0; i < sc.instances.size() && !full; ++i)
    {
        const scene::Instance& inst = sc.instances[i];
        if (inst.flags & (scene::InstanceSkinned | scene::InstanceWind)) continue;  // (deforming: no cards, as the reference)
        const scene::Mesh& mesh = sc.meshes[inst.mesh];
        if (mesh.positions.empty() || mesh.indices.empty() || !mesh.blendShapes.empty() || mesh.vertexAnimation.framesPerSecond > 0) continue;
        // rotation and uniform scale of the instance: mesh card space = the rotation's frame at the instance's origin, metres
        const float3 c0{ inst.transform.m[0][0], inst.transform.m[1][0], inst.transform.m[2][0] };
        const float3 c1{ inst.transform.m[0][1], inst.transform.m[1][1], inst.transform.m[2][1] };
        const float3 c2{ inst.transform.m[0][2], inst.transform.m[1][2], inst.transform.m[2][2] };
        const float scale = length(c0);
        if (!(scale > 0)) continue;
        const float3 rot[3] = { c0 / scale, c1 / scale, c2 / scale };  // the mesh card axes in world space
        const float3 origin{ inst.transform.m[0][3], inst.transform.m[1][3], inst.transform.m[2][3] };
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        for (const float3& p : mesh.positions)
        {
            lo = { std::min(lo.x, p.x * scale), std::min(lo.y, p.y * scale), std::min(lo.z, p.z * scale) };
            hi = { std::max(hi.x, p.x * scale), std::max(hi.y, p.y * scale), std::max(hi.z, p.z * scale) };
        }
        const float3 centre = (lo + hi) * 0.5f;
        const float3 half = (hi - lo) * 0.5f + float3{ kBoundsPad, kBoundsPad, kBoundsPad };

        MeshCardsRecord mc{};
        for (int r = 0; r < 3; ++r)
        {
            mc.worldToLocal[r][0] = rot[r].x, mc.worldToLocal[r][1] = rot[r].y, mc.worldToLocal[r][2] = rot[r].z;
            mc.worldToLocal[r][3] = r == 0 ? origin.x : r == 1 ? origin.y : origin.z;
        }
        mc.cardOffset = (uint32_t)cards.size();
        uint32_t count = 0;
        bool anyTwoSided = false;

        for (uint32_t d = 0; d < 6 && !full; ++d)
        {
            const uint32_t axis = d / 2;
            const float sign = (d & 1u) ? 1.0f : -1.0f;
            // the card's axes in mesh card space
            const float3 ax = axis == 0 ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
            const float3 ay = axis == 2 ? float3{ 0, 1, 0 } : float3{ 0, 0, 1 };
            const float3 az = axis == 0 ? float3{ sign, 0, 0 } : axis == 1 ? float3{ 0, sign, 0 } : float3{ 0, 0, sign };
            const float3 extent{ std::abs(dot(ax, half)), std::abs(dot(ay, half)), std::abs(dot(az, half)) };
            const float longest = std::max(extent.x, extent.y);
            const uint32_t longRes = std::clamp(pow2Ceil(kTexelsPerMetre * longest), kMinSide, kPage);
            const uint32_t w = extent.x >= extent.y ? longRes : std::clamp(pow2Ceil((float)longRes * extent.x / extent.y), kMinSide, kPage);
            const uint32_t h = extent.y >= extent.x ? longRes : std::clamp(pow2Ceil((float)longRes * extent.y / extent.x), kMinSide, kPage);
            cardDepth.assign((size_t)w * h, 2.0f);
            cardAlbedo.assign((size_t)w * h, 0);
            cardEmissive.assign((size_t)w * h, 0);
            cardNormal.assign((size_t)w * h, 0x8080);
            bool any = false;
            for (size_t sm = 0; sm < mesh.submeshes.size(); ++sm)
            {
                const scene::Submesh& sub = mesh.submeshes[sm];
                const uint32_t materialIndex = sm < inst.materialOverrides.size() ? inst.materialOverrides[sm] : sub.material;
                const scene::Material& mat = sc.materials[materialIndex];
                anyTwoSided = anyTwoSided || mat.twoSided;
                const float3 f0 = float3{ 0.04f, 0.04f, 0.04f } * (1 - mat.metallic) + mat.baseColor * mat.metallic;
                const float3 reflectance = mat.baseColor * (1 - mat.metallic) + f0 * 0.45f;
                const auto unorm8 = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                const uint32_t albedoTexel = unorm8(std::sqrt(std::clamp(reflectance.x, 0.0f, 1.0f))) | (unorm8(std::sqrt(std::clamp(reflectance.y, 0.0f, 1.0f))) << 8) |
                                             (unorm8(std::sqrt(std::clamp(reflectance.z, 0.0f, 1.0f))) << 16) | 0xFF000000u;
                const uint32_t emissiveTexel = mat.emissiveVisibleOnly ? 0u : packR11G11B10(mat.emissive * (1.0f / 16.0f));
                for (uint32_t t = 0; t + 2 < sub.indexCount; t += 3)
                {
                    float3 p[3], n[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint32_t v = mesh.indices[sub.indexOffset + t + k];
                        const float3 local = mesh.positions[v] * scale - centre;
                        p[k] = { dot(local, ax), dot(local, ay), dot(local, az) };
                        const float3 vn = v < mesh.normals.size() ? mesh.normals[v] : float3{ 0, 0, 0 };
                        n[k] = { dot(vn, ax), dot(vn, ay), dot(vn, az) };
                    }
                    // the triangle's own normal in mesh card space (counter-clockwise front), then along the card's axes
                    float3 q[3];
                    for (int k = 0; k < 3; ++k) q[k] = mesh.positions[mesh.indices[sub.indexOffset + t + k]] * scale;
                    const float3 gnMesh = cross(q[1] - q[0], q[2] - q[0]);
                    const float gl = length(gnMesh);
                    if (!(gl > 0)) continue;
                    float facing = dot(gnMesh, az) / gl;
                    if (mat.twoSided) facing = std::abs(facing);
                    if (facing < kNormalThreshold) continue;
                    // texel coordinates: uv = c.xy / extent.xy x 0.5 + 0.5
                    float sx[3], sy[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        sx[k] = (p[k].x / extent.x * 0.5f + 0.5f) * (float)w;
                        sy[k] = (p[k].y / extent.y * 0.5f + 0.5f) * (float)h;
                    }
                    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                    if (area == 0) continue;
                    const int x0 = std::max(0, (int)std::floor(std::min({ sx[0], sx[1], sx[2] }) - 0.5f));
                    const int x1 = std::min((int)w - 1, (int)std::ceil(std::max({ sx[0], sx[1], sx[2] }) - 0.5f));
                    const int y0 = std::max(0, (int)std::floor(std::min({ sy[0], sy[1], sy[2] }) - 0.5f));
                    const int y1 = std::min((int)h - 1, (int)std::ceil(std::max({ sy[0], sy[1], sy[2] }) - 0.5f));
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                            const float b0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                            const float b1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                            const float b2 = 1 - b0 - b1;
                            const float eps = -1e-4f;
                            if (b0 < eps || b1 < eps || b2 < eps) continue;
                            const float z = b0 * p[0].z + b1 * p[1].z + b2 * p[2].z;
                            const float dz = 0.5f - z / extent.z * 0.5f;  // 0: the card's front
                            const size_t at = (size_t)y * w + x;
                            if (dz >= cardDepth[at]) continue;
                            float3 nn = n[0] * b0 + n[1] * b1 + n[2] * b2;
                            const float nl = length(nn);
                            nn = nl > 1e-6f ? nn / nl : float3{ 0, 0, 1 };
                            if (mat.twoSided && nn.z < 0) nn = -nn;
                            if (nn.z < 0) nn = float3{ 0, 0, 1 };  // (a vertex normal turned away from the face)
                            cardDepth[at] = dz;
                            cardAlbedo[at] = albedoTexel;
                            cardEmissive[at] = emissiveTexel;
                            const auto unorm = [](float v) { return (uint32_t)std::lround(std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f); };
                            cardNormal[at] = (uint16_t)(unorm(nn.x) | (unorm(nn.y) << 8));
                            any = true;
                        }
                }
            }
            if (!any) continue;
            // a place in the atlas
            if (shelfX + w > kAtlas)
            {
                shelfX = 0;
                shelfY += shelfHeight;
                shelfHeight = 0;
            }
            if (shelfY + h > kAtlas)
            {
                full = true;
                break;
            }
            const uint32_t atX = shelfX, atY = shelfY;
            shelfX += w;
            shelfHeight = std::max(shelfHeight, h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const size_t from = (size_t)y * w + x, to = (size_t)(atY + y) * kAtlas + atX + x;
                    const float dz = cardDepth[from];
                    depth[to] = dz > 1.5f ? 0xFFFF : (uint16_t)std::min(65534.0f, std::round(std::clamp(dz, 0.0f, 1.0f) * 65535.0f));
                    albedo[to] = cardAlbedo[from];
                    emissive[to] = cardEmissive[from];
                    normal[to] = cardNormal[from];
                }
            const uint32_t cardIndex = (uint32_t)cards.size(), pageIndex = (uint32_t)cardPages.size();
            CardRecord card{};
            card.origin[0] = centre.x, card.origin[1] = centre.y, card.origin[2] = centre.z;
            card.packed = d | 0x10000u;
            card.extent[0] = extent.x, card.extent[1] = extent.y, card.extent[2] = extent.z;
            card.texelSize = 2 * longest / (float)longRes;
            card.sizeInPages = card.hiResSizeInPages = 1u | (1u << 16);
            card.pageTableOffset = card.hiResPageTableOffset = pageIndex;
            const float3 worldCentre = origin + rot[0] * centre.x + rot[1] * centre.y + rot[2] * centre.z;
            const float3 wx = rot[0] * ax.x + rot[1] * ax.y + rot[2] * ax.z, wy = rot[0] * ay.x + rot[1] * ay.y + rot[2] * ay.z,
                         wz = rot[0] * az.x + rot[1] * az.y + rot[2] * az.z;
            const float rows[3][4] = { { wx.x, wy.x, wz.x, worldCentre.x }, { wx.y, wy.y, wz.y, worldCentre.y }, { wx.z, wy.z, wz.z, worldCentre.z } };
            std::memcpy(card.cardToWorld, rows, sizeof rows);
            card.meshCards = (uint32_t)meshCards.size();
            cards.push_back(card);
            CardPageRecord page{};
            page.card = cardIndex;
            page.resLevelPageTableOffset = pageIndex;
            page.sizeInTexels[0] = (float)w, page.sizeInTexels[1] = (float)h;
            page.cardUvRect[0] = page.cardUvRect[1] = 0, page.cardUvRect[2] = page.cardUvRect[3] = 1;
            page.atlasRect[0] = (float)atX, page.atlasRect[1] = (float)atY, page.atlasRect[2] = (float)(atX + w), page.atlasRect[3] = (float)(atY + h);
            page.cardUvTexelScale[0] = 1.0f / (float)w, page.cardUvTexelScale[1] = 1.0f / (float)h;
            page.resLevelSizeInTiles = (w / 8) | ((h / 8) << 16);
            cardPages.push_back(page);
            pageTable.push_back((atX / 8) | ((atY / 8) << 12) | (log2Of(w) << 24) | (log2Of(h) << 28));
            pageTable.push_back(pageIndex);
            mc.cardLookup[d] |= 1u << count;
            ++count;
        }
        if (count == 0)
        {
            ++skipped;
            continue;
        }
        mc.countFlags = count | (anyTwoSided ? 0x20000u : 0u);
        instanceMap[i] = (uint32_t)meshCards.size();
        meshCards.push_back(mc);
    }
    logf("card test set: %zu instances, %zu with cards (%u without a facing triangle), %zu cards, atlas rows used %u of %u%s\n", sc.instances.size(), meshCards.size(), skipped,
         cards.size(), shelfY + shelfHeight, kAtlas, full ? " - ATLAS FULL, the rest of the scene has no cards" : "");

    // ---- GPU resources and the staging image
    const void* data[5] = { instanceMap.data(), meshCards.data(), cards.data(), cardPages.data(), pageTable.data() };
    const uint64_t bytes[5] = { instanceMap.size() * 4, meshCards.size() * sizeof(MeshCardsRecord), cards.size() * sizeof(CardRecord), cardPages.size() * sizeof(CardPageRecord),
                                pageTable.size() * 4 };
    static const wchar_t* const kBufferNames[5] = { L"R card test instance map", L"R card test mesh cards", L"R card test cards", L"R card test pages", L"R card test page table" };
    uint64_t total = 0;
    for (int b = 0; b < 5; ++b)
    {
        if (buffers[b]) device.deferRelease(buffers[b]);
        bufferBytes[b] = (std::max<uint64_t>(bytes[b], 16) + 15) & ~15ull;
        buffers[b] = makeBuffer(device, bufferBytes[b], D3D12_HEAP_TYPE_DEFAULT, kBufferNames[b]);
        bufferOffset[b] = total;
        total += bufferBytes[b];
    }
    static const DXGI_FORMAT kFormats[4] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R11G11B10_FLOAT };
    static const wchar_t* const kTextureNames[4] = { L"R card test depth", L"R card test albedo", L"R card test normal", L"R card test emissive" };
    const void* texels[4] = { depth.data(), albedo.data(), normal.data(), emissive.data() };
    const uint32_t texelBytes[4] = { 2, 4, 2, 4 };
    for (int t = 0; t < 4; ++t)
    {
        if (!textures[t]) textures[t] = makeTexture(device, kFormats[t], kTextureNames[t]);
        const D3D12_RESOURCE_DESC desc = textures[t]->GetDesc();
        total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        uint64_t size = 0;
        device.d3d()->GetCopyableFootprints(&desc, 0, 1, total, &footprint[t], nullptr, nullptr, &size);
        total += size;
    }
    if (staging) device.deferRelease(staging);
    staging = makeBuffer(device, total, D3D12_HEAP_TYPE_UPLOAD, L"R card test staging");
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map card test staging");
    for (int b = 0; b < 5; ++b)
    {
        std::memset(mapped + bufferOffset[b], 0, bufferBytes[b]);
        if (bytes[b]) std::memcpy(mapped + bufferOffset[b], data[b], bytes[b]);
    }
    for (int t = 0; t < 4; ++t)
        for (uint32_t y = 0; y < kAtlas; ++y)
            std::memcpy(mapped + footprint[t].Offset + (uint64_t)y * footprint[t].Footprint.RowPitch, static_cast<const uint8_t*>(texels[t]) + (size_t)y * kAtlas * texelBytes[t],
                        (size_t)kAtlas * texelBytes[t]);
    staging->Unmap(0, nullptr);
    pages = (uint32_t)cardPages.size();
    instances = (uint32_t)instanceMap.size();
    pending = true;
    ++generation;
}

CardSet CardTestSet::record(FramePassContext& fc)
{
    Impl& s = *m;
    if (s.revision != fc.scene.revision() || !s.buffers[0])
    {
        s.build(fc.scene);
        s.revision = fc.scene.revision();
    }
    RenderGraph& g = fc.graph;
    static const char* const kBufferNames[5] = { "R card test instance map", "R card test mesh cards", "R card test cards", "R card test pages", "R card test page table" };
    static const char* const kTextureNames[4] = { "R card test depth", "R card test albedo", "R card test normal", "R card test emissive" };
    static const DXGI_FORMAT kFormats[4] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R11G11B10_FLOAT };
    BufferRef buffers[5];
    TextureRef textures[4];
    for (int b = 0; b < 5; ++b) buffers[b] = g.importBuffer(s.buffers[b].Get(), { kBufferNames[b], s.bufferBytes[b], 0 });
    for (int t = 0; t < 4; ++t) textures[t] = g.importTexture(s.textures[t].Get(), { kTextureNames[t], kAtlas, kAtlas, 1, 1, kFormats[t] }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    if (s.pending)
    {
        ID3D12Resource* staging = s.staging.Get();
        std::array<uint64_t, 5> offsets, sizes;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints;
        for (int b = 0; b < 5; ++b) offsets[b] = s.bufferOffset[b], sizes[b] = s.bufferBytes[b];
        for (int t = 0; t < 4; ++t) footprints[t] = s.footprint[t];
        const std::array<BufferRef, 5> bufferRefs = { buffers[0], buffers[1], buffers[2], buffers[3], buffers[4] };
        const std::array<TextureRef, 4> textureRefs = { textures[0], textures[1], textures[2], textures[3] };
        g.addPass("r.card.testset.upload", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      for (const BufferRef& r : bufferRefs) b.use(r, Use::CopyDst);
                      for (const TextureRef& r : textureRefs) b.use(r, Use::CopyDst);
                      b.keep();
                  },
                  [staging, offsets, sizes, footprints, bufferRefs, textureRefs](PassContext& c) {
                      for (int b = 0; b < 5; ++b) c.cmd->CopyBufferRegion(c.resource(bufferRefs[b]), 0, staging, offsets[b], sizes[b]);
                      for (int t = 0; t < 4; ++t)
                      {
                          D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                          to.pResource = c.resource(textureRefs[t]);
                          to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                          from.pResource = staging;
                          from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                          from.PlacedFootprint = footprints[t];
                          c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                      }
                  });
        s.pending = false;  // (the staging buffer stays until the next build)
    }
    CardSet set;
    set.valid = s.pages > 0;
    set.instanceMap = buffers[0], set.meshCards = buffers[1], set.cards = buffers[2], set.cardPages = buffers[3], set.pageTable = buffers[4];
    set.depth = textures[0], set.albedo = textures[1], set.normal = textures[2], set.emissive = textures[3];
    set.atlasSize = kAtlas;
    set.cardPageCapacity = std::max(s.pages, 1u);
    set.cardPageCount = s.pages;
    set.instances = s.instances;
    set.generation = s.generation;
    return set;
}
} // namespace unx::render::refl
