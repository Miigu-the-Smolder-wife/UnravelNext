// Mesh card records and atlases as agreed in Docs/Status/MESH_CARDS_INTERFACE_KO.md sections 2-4 (A owns the format and
// will give Passes/SurfaceCache/MeshCards.hlsli with these loaders; this file is S2's stand-in until that header lands,
// written from the same section so the names and layouts are the agreed ones - it is then removed and the includes
// change). Everything S2's lighting and hit read need of a card set is reached from one small raw buffer, the card
// frame (CardLighting.cpp writes it each frame):
//   word 0 instance map SRV (uint per scene instance: its mesh cards, 0xFFFFFFFF none), 1 mesh cards SRV (McMeshCards),
//   2 cards SRV (McCard), 3 card pages SRV (McCardPage), 4 page table SRV (uint2), 5 depth atlas SRV, 6 albedo atlas SRV,
//   7 normal atlas SRV, 8 emissive atlas SRV, 9 atlas size (texels, square), 10 card pages in use, 11 frame index,
//   12 final lighting atlas SRV, 13 direct lighting atlas SRV, 14 indirect lighting atlas SRV, 15 page light state SRV
//   (CardLighting.hlsli), 16 scene instances in the map, 17 asuint(the hit read's depth bias, m),
//   18 feedback table UAV (raw; MC_NONE: no feedback - CardLighting.hlsli clFeedback), 19 the feedback's dither: tile
//   jitter x | y << 8 | tile mask << 16, 20 asuint(the feedback's resolution level bias), 21 last-used UAV (raw: per
//   card page the update in which a reader of the high levels last read it; MC_NONE: none),
//   22..27 the indirect light of hits without cards (Passes/GI/LumenHitIndirect.hlsli lhiSources; written with the
//   frame and again by the final gather once this frame's volume and radiance cache stand: CardFrameSources.hlsl),
//   28..39 the rules the frame's ray hits share (LumenHitIndirect.hlsli lhiRules): 28 asuint(the far field's start, m:
//   the mesh cards' end; 0: none), 29 asuint(1 / the skylight leaking's full distance, 1/m), 30 asuint(the distant
//   screen traces' length past the rays' end, m; 0: none), 31 asuint(their slope compare tolerance), 32..34
//   asuint(the skylight leaking's colour; 0: none), 35 asuint(its share at the reflections' hits), 36 asuint(the
//   distant screen traces' step offset bias), 37..39 unused.
// The record buffers are raw here (the loaders hide it).
#ifndef UNX_CARD_LAYOUT_HLSLI
#define UNX_CARD_LAYOUT_HLSLI
#include "Bindless.hlsli"

#define MC_NONE 0xFFFFFFFFu
#define MC_TILE 8u              // card tile, texels
#define MC_PAGE 128u            // physical page, texels
#define MC_VIRTUAL_PAGE 127u    // the card texels a page holds (half a texel of border between pages)
#define MC_EMISSIVE_SCALE (1.0 / 16.0)
#define MC_DEPTH_NONE 1.0       // no surface in the texel
#define MC_MIN_RES_LEVEL 3u     // a card's resolution levels: log2 of the texels along its longer side (mc::kMinResLevel)
#define MC_MAX_RES_LEVEL 11u
#define MC_SUB_ALLOC_RES_LEVEL 7u  // log2(MC_PAGE): a level up to this is one element inside a shared physical page
#define MC_FRAME_SOURCES 88u    // byte offset of the frame's words 22..27 (LumenHitIndirect.hlsli)
#define MC_FRAME_RULES 112u     // byte offset of the frame's words 28..39 (LumenHitIndirect.hlsli lhiRules)

struct McFrame
{
    uint instanceMap, meshCards, cards, cardPages, pageTable;
    uint depth, albedo, normal, emissive;
    uint atlasSize, pageCount, frame;
    uint finalLighting, directLighting, indirectLighting, pageLight;
    uint instances;
    float depthBias;
    uint feedback, feedbackDither;
    float feedbackBias;
    uint lastUsed;
};
McFrame mcFrame(uint frameSrv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[frameSrv];
    const uint4 a = b.Load4(0), c = b.Load4(16), d = b.Load4(32), e = b.Load4(48);
    McFrame f;
    f.instanceMap = a.x, f.meshCards = a.y, f.cards = a.z, f.cardPages = a.w;
    f.pageTable = c.x, f.depth = c.y, f.albedo = c.z, f.normal = c.w;
    f.emissive = d.x, f.atlasSize = d.y, f.pageCount = d.z, f.frame = d.w;
    f.finalLighting = e.x, f.directLighting = e.y, f.indirectLighting = e.z, f.pageLight = e.w;
    const uint4 g = b.Load4(64);
    f.instances = g.x;
    f.depthBias = asfloat(g.y);
    f.feedback = g.z, f.feedbackDither = g.w;
    const uint2 h = b.Load2(80);
    f.feedbackBias = asfloat(h.x);
    f.lastUsed = h.y;
    return f;
}

struct McMeshCards  // 80 B
{
    float4 worldToLocal[3];  // xyz: the rows of the unit rotation world -> mesh card space, w: the world origin's component
    uint cardOffset;
    uint countFlags;         // bits 0-15 cards (<= 32), bit 17 mostly two-sided
    uint cardLookup[6];      // per direction d (-X, +X, -Y, +Y, -Z, +Z): bit i = card i of this mesh faces it
};
struct McCard  // 112 B
{
    float3 origin;           // box centre, mesh card space
    uint packed;             // bits 0-2 direction, 4-7 resLevelBiasX, 8-11 resLevelBiasY, 16 visible
    float3 extent;           // half sizes along the card's axes (x, y: the card's face; z: depth)
    float texelSize;
    uint sizeInPages;        // resident level: x | y << 16
    uint pageTableOffset;
    uint hiResSizeInPages;
    uint hiResPageTableOffset;
    float4 cardToWorld[3];   // xyz: the world components of the card's X, Y, Z axes (row = world x, y, z), w: the centre
    uint meshCards;
    uint pad0, pad1, pad2;
};
struct McCardPage  // 64 B
{
    uint card;
    uint resLevelPageTableOffset;
    float2 sizeInTexels;     // x == 0: not mapped
    float4 cardUvRect;       // the card uv this page holds (min xy, max zw)
    float4 atlasRect;        // atlas texels (min xy, max zw)
    float2 cardUvTexelScale;
    uint resLevelSizeInTiles;  // x | y << 16
    uint pad;
};

McMeshCards mcLoadMeshCards(McFrame f, uint i)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[f.meshCards];
    return b.Load<McMeshCards>(i * 80);
}
McCard mcLoadCard(McFrame f, uint i)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[f.cards];
    return b.Load<McCard>(i * 112);
}
McCardPage mcLoadCardPage(McFrame f, uint i)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[f.cardPages];
    return b.Load<McCardPage>(i * 64);
}
uint mcMeshCardsOf(McFrame f, uint sceneInstance)
{
    if (sceneInstance >= f.instances) return MC_NONE;
    ByteAddressBuffer b = ResourceDescriptorHeap[f.instanceMap];
    return b.Load(sceneInstance * 4);
}
uint mcDirection(McCard c) { return c.packed & 7u; }
bool mcVisible(McCard c) { return (c.packed & 0x10000u) != 0; }
uint2 mcResLevelSizeInTiles(McCardPage p) { return uint2(p.resLevelSizeInTiles & 0xFFFFu, p.resLevelSizeInTiles >> 16); }

// World -> card space (the card's axes; the origin at the card's centre) and back.
float3 mcWorldToCard(McCard c, float3 world)
{
    const float3 d = world - float3(c.cardToWorld[0].w, c.cardToWorld[1].w, c.cardToWorld[2].w);
    return float3(dot(d, float3(c.cardToWorld[0].x, c.cardToWorld[1].x, c.cardToWorld[2].x)),
                  dot(d, float3(c.cardToWorld[0].y, c.cardToWorld[1].y, c.cardToWorld[2].y)),
                  dot(d, float3(c.cardToWorld[0].z, c.cardToWorld[1].z, c.cardToWorld[2].z)));
}
float3 mcCardToWorldVector(McCard c, float3 v)
{
    return float3(dot(c.cardToWorld[0].xyz, v), dot(c.cardToWorld[1].xyz, v), dot(c.cardToWorld[2].xyz, v));
}
float3 mcCardToWorld(McCard c, float3 local) { return mcCardToWorldVector(c, local) + float3(c.cardToWorld[0].w, c.cardToWorld[1].w, c.cardToWorld[2].w); }
float3 mcWorldToCardVector(McCard c, float3 v)
{
    return float3(dot(v, float3(c.cardToWorld[0].x, c.cardToWorld[1].x, c.cardToWorld[2].x)),
                  dot(v, float3(c.cardToWorld[0].y, c.cardToWorld[1].y, c.cardToWorld[2].y)),
                  dot(v, float3(c.cardToWorld[0].z, c.cardToWorld[1].z, c.cardToWorld[2].z)));
}

// Card uv and depth of a card-space point: uv = c.xy / extent.xy x 0.5 + 0.5; depth = 0.5 - c.z / extent.z x 0.5 (0: the
// card's front, the side it was captured from; 1: its back).
float2 mcCardUv(McCard c, float3 local) { return local.xy / c.extent.xy * 0.5 + 0.5; }
float mcCardDepth(McCard c, float3 local) { return 0.5 - local.z / c.extent.z * 0.5; }
// The world point of a card texel (its uv and stored depth).
float3 mcCardWorldPosition(McCard c, float2 uv, float depth)
{
    return mcCardToWorld(c, float3((uv * 2 - 1) * c.extent.xy, (0.5 - depth) * 2 * c.extent.z));
}

float3 mcDecodeAlbedo(float3 stored) { return stored * stored; }
// The surface normal of a card texel (world): the atlas holds the card-space normal's xy x 0.5 + 0.5, z >= 0.
float3 mcDecodeNormal(McCard c, float2 stored)
{
    const float2 xy = stored * 2 - 1;
    return normalize(mcCardToWorldVector(c, float3(xy, sqrt(saturate(1 - dot(xy, xy))))));
}
float3 mcDecodeEmissive(float3 stored) { return stored / MC_EMISSIVE_SCALE; }

// A texel of a card page: its atlas coordinate, card uv, and what the geometry atlases hold there.
struct McTexel
{
    bool valid;        // inside the page and a surface in the texel
    uint2 atlas;
    float2 cardUv;
    float3 position;   // world
    float3 normal;     // world
};
McTexel mcPageTexel(McFrame f, McCardPage page, McCard card, uint2 coordInPage)
{
    McTexel t;
    t.valid = false;
    t.atlas = uint2(page.atlasRect.xy) + coordInPage;
    t.cardUv = page.cardUvRect.xy + page.cardUvTexelScale * (float2(coordInPage) + 0.5);
    t.position = t.normal = 0;
    if (any(float2(coordInPage) >= page.sizeInTexels)) return t;
    Texture2D<float> depth = ResourceDescriptorHeap[f.depth];
    const float d = depth.Load(int3(t.atlas, 0));
    if (!(d < MC_DEPTH_NONE)) return t;
    Texture2D<float2> normal = ResourceDescriptorHeap[f.normal];
    t.valid = true;
    t.position = mcCardWorldPosition(card, t.cardUv, d);
    t.normal = mcDecodeNormal(card, normal.Load(int3(t.atlas, 0)));
    return t;
}

// Where a card-space point of a card lands in the atlas (the reference's ComputeSurfaceCacheSample): the four texels
// around it and their bilinear weights. valid = false when the page is not mapped. hiRes false: the card's resident
// (locked) level, every page of which is mapped. hiRes true: its highest level - there only the pages the feedback
// asked for are mapped, and the page table sends every other page to the nearest lower level's page that is; the
// sample is laid out in whichever page that is (its own rectangle of the card).
struct McCardSample
{
    bool valid;
    uint page;          // card page index
    uint2 texel00;      // atlas texel of the lower-left of the four
    float4 weights;     // (0,0) (1,0) (0,1) (1,1)
    uint2 tile;         // atlas coordinate / MC_TILE of the sample
};
McCardSample mcCardSample(McFrame f, McCard card, float2 localXy, bool hiRes = false)
{
    McCardSample s;
    s.valid = false;
    s.page = 0;
    s.texel00 = 0;
    s.weights = 0;
    s.tile = 0;
    const uint packedSize = hiRes ? card.hiResSizeInPages : card.sizeInPages;
    const uint2 sizeInPages = uint2(packedSize & 0xFFFFu, packedSize >> 16);
    if (any(sizeInPages == 0)) return s;
    float2 uv = saturate(localXy / card.extent.xy * 0.5 + 0.5);
    const uint2 pageCoord = min(uint2(uv * float2(sizeInPages)), sizeInPages - 1);
    ByteAddressBuffer table = ResourceDescriptorHeap[f.pageTable];
    const uint2 entry = table.Load2(((hiRes ? card.hiResPageTableOffset : card.pageTableOffset) + pageCoord.x + pageCoord.y * sizeInPages.x) * 8);
    const uint2 resLevel = uint2((entry.x >> 24) & 0xFu, entry.x >> 28);
    if (resLevel.x == 0) return s;
    s.page = entry.y;
    const McCardPage page = mcLoadCardPage(f, s.page);
    // inside the page: half a texel in from its edges (pages of a card overlap by the border, sub-allocated cards end there)
    const float2 pageUv = saturate((uv - page.cardUvRect.xy) / max(page.cardUvRect.zw - page.cardUvRect.xy, 1e-8));
    const float2 texels = clamp(pageUv * page.sizeInTexels, 0.5, page.sizeInTexels - 0.5 - 1.0 / 512.0) - 0.5;
    const float2 base = floor(texels), frac2 = texels - base;
    s.texel00 = uint2(page.atlasRect.xy + base);
    s.weights = float4((1 - frac2.x) * (1 - frac2.y), frac2.x * (1 - frac2.y), (1 - frac2.x) * frac2.y, frac2.x * frac2.y);
    s.tile = uint2(page.atlasRect.xy + texels + 0.5) / MC_TILE;
    s.valid = true;
    return s;
}

#endif
