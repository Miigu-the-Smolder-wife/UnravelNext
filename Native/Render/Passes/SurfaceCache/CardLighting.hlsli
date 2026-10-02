// The surface cache's lighting on mesh cards (surface_cache.mesh_cards; S2 - Docs/Status/MESH_CARDS_INTERFACE_KO.md
// section 9; the structure of the reference's card lighting: update selection by page priority, direct light per card
// tile, radiosity per card probe, the final lighting atlas, and the hit read that samples up to the cards of a mesh that
// face the hit's normal).
// Atlases (the card atlas' size, one texel per card texel):
//   direct     R11G11B10F  irradiance of the local lights and the sun, lux x CL_IRRADIANCE_SCALE
//   indirect   R11G11B10F  irradiance of the radiosity, lux x CL_IRRADIANCE_SCALE
//   final      R11G11B10F  (direct + indirect) x albedo / pi + emission, nits x CL_RADIANCE_SCALE - what a hit reads
//   trace      R11G11B10F  the radiosity rays' radiance, nits x CL_RADIANCE_SCALE: probe (4 x 4 texels) x 4 x 4 rays
//   SH 0..2    RGBA16F     atlas / 4: a probe's filtered radiance as 4 SH coefficients per colour (nits)
//   frames     R8_UINT     atlas / 8: radiosity frames accumulated in the tile (<= radiosity_max_frames_accumulated)
// Buffers:
//   page light (16 B a card page): { last direct update frame + 1 (0: never), last indirect update frame + 1, direct
//              temporal index (bit 31, CL_PAGE_ANIMATED: at the page's last direct update one of its lights had a
//              function that changes with time), indirect temporal index }
//   uniform    (32 B an atlas tile): bit (light & 255) = the light's visibility was the same over the whole tile at the
//              tile's last direct update (the next one traces one ray per 2 x 2 texels); bit 255: the sun
//   last used  (4 B a card page): the update in which a reader of the cards' high levels last read the page (clFeedback);
//              the selection relights such pages first (surface_cache.lighting_feedback)
//   feedback   (the readers'; clFeedback): a hash table of (card page of a level, hits) the frame's hits asked for
//   select     (the frame's; CL_SELECT_*): the priority histograms, the buckets chosen, the tile lists of this frame's
//              direct and radiosity updates
#ifndef UNX_CARD_LIGHTING_HLSLI
#define UNX_CARD_LIGHTING_HLSLI
#include "Passes/SurfaceCache/CardLayout.hlsli"

#define CL_IRRADIANCE_SCALE (1.0 / 64.0)
#define CL_RADIANCE_SCALE (1.0 / 16.0)
#define CL_LIGHTS 8u                 // lights of a card tile (the reference's MaxLightsPerTile)
#define CL_SLOTS 9u                  // the lights, then the sun
#define CL_SUN_SLOT 8u
#define CL_SUN_BIT 255u
#define CL_PROBE_SPACING 4u          // texels between radiosity probes
#define CL_PROBE_RAYS 4u             // rays of a probe along one side of its hemisphere map
#define CL_BUCKETS 16u               // priority histogram size
#define CL_NEVER_FRAMES 2048.0       // a page never lit counts as this many frames old
#define CL_PAGE_LIGHT_BYTES 16u
#define CL_PAGE_ANIMATED 0x80000000u // in a page light's direct index: lit by a light whose function changes with time
#define CL_UNIFORM_BYTES 32u
#define CL_TILE_LIGHT_BYTES 64u      // a listed tile's lights: 8 light indices, valid mask (2), uniform slots, pad
#define CL_TILE_SHADOW_BYTES 72u     // a listed tile's visible bits: CL_SLOTS x 64 texels
#define CL_TRACE_THREADS 576u        // CL_SLOTS x 64: the direct trace's threads of a listed tile
// The direct light through Glass (surface_cache.direct_tint): beside a listed tile's visible bits, per tinted slot 64
// words - what the Glass on each texel's shadow ray left of the light (RayShaders.hlsli rtShadowTransmittance), 11 : 11 :
// 10 bits as the view's sun tint (VsmTint.hlsli vsmTintPack; white = 0xFFFFFFFF). The tinted slots of a tile: the sun
// alone (1: 256 B a tile), or every slot (CL_SLOTS: 2,304 B a tile - surface_cache.direct_tint_lights). A word is
// written by the texel's trace thread before its visible bit and read only where that bit is set: nothing is cleared.
#define CL_TILE_TINT_WORDS 64u
#define CL_FEEDBACK_SLOTS 4096u      // the feedback table's hash slots (a power of two; SurfaceCacheCards.cpp)
#define CL_FEEDBACK_HEAD 16u         // its header, bytes: word 0 = inserts that found no slot (the table overflowed)
#define CL_FEEDBACK_PROBES 8u        // the structural bound of an insert's linear probe
#define CL_FEEDBACK_CARDS 0x100000u  // an element holds the card in 20 bits: cards past it report nothing

uint clTintPack(float3 t)
{
    const uint3 q = uint3(round(saturate(t) * float3(2047.0, 2047.0, 1023.0)));
    return q.x | (q.y << 11) | (q.z << 22);
}
float3 clTintUnpack(uint w) { return float3(w & 0x7FFu, (w >> 11) & 0x7FFu, w >> 22) / float3(2047.0, 2047.0, 1023.0); }
// Byte offset of a texel's tint word: index = the listed tile, tintSlots = the tile's tinted slots (1: the sun's, slot
// ignored; CL_SLOTS: every slot's).
uint clTintOffset(uint index, uint tintSlots, uint slot, uint t) { return ((index * tintSlots + (tintSlots > 1u ? slot : 0u)) * CL_TILE_TINT_WORDS + t) * 4u; }

// ---- the select buffer (raw). Context 0: direct, 1: radiosity.
#define CL_SELECT_HEAD 64u                                   // 8 words a context: tiles listed, max bucket, tiles allowed
                                                             // from the max bucket, tiles reserved in it, pages listed
uint clSelectContext(uint context) { return context * 32u; }
#define CL_SELECT_TILES 0u
#define CL_SELECT_MAX_BUCKET 4u
#define CL_SELECT_ALLOWED 8u
#define CL_SELECT_RESERVED 12u
#define CL_SELECT_PAGES 16u
uint clHistogramOffset(uint context, uint bucket) { return CL_SELECT_HEAD + (context * CL_BUCKETS + bucket) * 4u; }
uint clPageBucketOffset(uint page) { return CL_SELECT_HEAD + 2u * CL_BUCKETS * 4u + page * 4u; }
// (lists: after the page buckets of 'pageCapacity' pages; a context's list holds listCapacity[context] tiles)
uint clTileListOffset(uint pageCapacity, uint directCapacity, uint context, uint index)
{
    return clPageBucketOffset(pageCapacity) + (context * directCapacity + index) * 4u;
}
// A listed tile: its card page and its tile inside the page.
uint clPackTile(uint page, uint2 tile) { return page | (tile.x << 24) | (tile.y << 28); }
void clUnpackTile(uint packed, out uint page, out uint2 tile)
{
    page = packed & 0xFFFFFFu;
    tile = uint2((packed >> 24) & 0xFu, packed >> 28);
}

struct ClPageLight
{
    uint directFrame, indirectFrame;  // last update frame + 1; 0: never
    uint directIndex, indirectIndex;  // updates so far (the jitter's index)
};
ClPageLight clPageLight(uint4 w)
{
    ClPageLight p;
    p.directFrame = w.x, p.indirectFrame = w.y, p.directIndex = w.z, p.indirectIndex = w.w;
    return p;
}

uint clHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float clUnit(uint x) { return (clHash(x) >> 8) * (1.0 / 16777216.0); }

// The texel a probe stands on this update: the probe's 4 x 4 texels, one chosen by the page's temporal index (a 16-point
// Hammersley set, so 16 updates visit 16 different texels).
uint2 clProbeTexelOffset(uint temporalIndex)
{
    const uint i = temporalIndex & 15u;
    const uint r = ((i & 1u) << 3) | ((i & 2u) << 1) | ((i & 4u) >> 1) | ((i & 8u) >> 3);  // radical inverse, 4 bits
    return uint2(i >> 2, r >> 2) & 3u;
}

// A direction of the upper hemisphere about n, uniform in solid angle (pdf 1 / 2 pi).
float3 clHemisphereDirection(float3 n, float2 u)
{
    const float z = u.x, r = sqrt(saturate(1 - z * z)), phi = 6.28318530718 * u.y;
    const float s = n.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    const float3 t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    const float3 b = float3(c, s + n.y * n.y * a, -n.y);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * z);
}
// The ray of a probe: trace texel (0..3)^2 of its hemisphere map, jittered inside the texel per probe and update.
float3 clProbeRayDirection(float3 normal, uint2 probeAtlasCoord, uint2 rayCoord, uint temporalIndex)
{
    const uint seed = clHash(probeAtlasCoord.x + probeAtlasCoord.y * 4099u + temporalIndex * 786433u);
    const float2 u = (float2(rayCoord) + float2(clUnit(seed), clUnit(seed ^ 0x9e3779b9u))) / CL_PROBE_RAYS;
    return clHemisphereDirection(normal, u);
}

// SH (4 coefficients): basis and the irradiance of a normal from a radiance projection.
float4 clShBasis(float3 d) { return float4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x); }
float4 clShIrradianceTransfer(float3 n) { return float4(3.14159265 * 0.282095, (2.0943951 * 0.488603) * n.y, (2.0943951 * 0.488603) * n.z, (2.0943951 * 0.488603) * n.x); }

// A probe of a card's resolution level (probe coordinates run over the whole level, pages 128 texels apart): the page that
// holds it - found through the level's page table, so a neighbour probe may lie in another page of the card - and the
// texel it stands on there (the page's own temporal index). Not valid: outside the level, a page that is not mapped or
// has had no radiosity update, no surface in the probe's texel.
struct ClProbe
{
    bool valid;
    uint2 atlasProbe;   // atlas texel / CL_PROBE_SPACING: the probe's place in the trace atlas (x 4) and the SH atlases
    McTexel texel;
};
uint2 clPageOriginInCard(McCardPage page) { return uint2(page.cardUvRect.xy * float2(mcResLevelSizeInTiles(page)) + 0.5) * MC_TILE; }
ClProbe clProbeAt(McFrame f, McCard card, McCardPage from, uint fromIndex, ByteAddressBuffer pageLight, int2 probeInCard)
{
    ClProbe o;
    o.valid = false;
    o.atlasProbe = 0;
    o.texel = (McTexel)0;
    const uint2 sizeInTexels = mcResLevelSizeInTiles(from) * MC_TILE;
    if (any(probeInCard < 0) || any(uint2(probeInCard) * CL_PROBE_SPACING >= sizeInTexels)) return o;
    const uint2 coordInCard = uint2(probeInCard) * CL_PROBE_SPACING;
    const uint2 pageCoord = coordInCard / MC_PAGE, sizeInPages = (sizeInTexels + MC_PAGE - 1) / MC_PAGE;
    uint pageIndex = fromIndex;
    McCardPage page = from;
    if (any(pageCoord != clPageOriginInCard(from) / MC_PAGE))
    {
        ByteAddressBuffer table = ResourceDescriptorHeap[f.pageTable];
        const uint2 entry = table.Load2((from.resLevelPageTableOffset + pageCoord.x + pageCoord.y * sizeInPages.x) * 8);
        if (((entry.x >> 24) & 0xFu) == 0) return o;
        pageIndex = entry.y;
        page = mcLoadCardPage(f, pageIndex);
        if (!(page.sizeInTexels.x > 0)) return o;
        // (a level above the resident one holds only the pages the feedback asked for: the table sends the others to a
        // lower level's page, whose probes are not this level's)
        if (page.resLevelPageTableOffset != from.resLevelPageTableOffset) return o;
    }
    const uint2 lightState = pageLight.Load2(pageIndex * CL_PAGE_LIGHT_BYTES + 4).xy;  // { indirect frame, direct index }
    if (lightState.x == 0) return o;
    const uint temporalIndex = pageLight.Load(pageIndex * CL_PAGE_LIGHT_BYTES + 12);
    const uint2 coordInPage = coordInCard - pageCoord * MC_PAGE + clProbeTexelOffset(temporalIndex);
    if (any(float2(coordInPage) >= page.sizeInTexels)) return o;
    o.texel = mcPageTexel(f, page, card, coordInPage);
    o.atlasProbe = (uint2(page.atlasRect.xy) + coordInPage) / CL_PROBE_SPACING;
    o.valid = o.texel.valid;
    return o;
}
// Whether a probe may be used at a point (the reference's plane weight, depth scale -100): the point's distance from the
// probe to its own tangent plane, relative to its distance from the probe.
float clPlaneWeight(float3 position, float3 normal, float3 probePosition)
{
    const float planeDistance = abs(dot(probePosition - position, normal));
    const float relative = max(planeDistance / (length(probePosition - position) + 0.01), 0.1);
    return exp2(-100.0 * relative * relative) > 0.01 ? 1.0 : 0.0;
}

// ---- the hit read (the reference's SampleLumenMeshCards order; the interface's section 4, steps 1-7).
#define CL_READ_IRRADIANCE 1u   // direct and indirect irradiance (the hit shades with its own material)
#define CL_READ_FINAL 2u        // the final lighting (the card's own albedo and emission)
#define CL_READ_MATERIAL 4u     // the card's albedo and emission
struct ClSample
{
    bool valid;
    float3 direct, indirect;    // lux (the sun is in direct)
    float3 final;               // nits
    float3 albedo, emission;
};
// Feedback (the reference's surface cache feedback, LumenSurfaceCacheSampling.ush + LumenSurfaceCacheFeedback.cpp):
// a hit that read a card reports which page of which resolution level it wanted - the level whose texel is the hit's
// footprint: log2(the card's half size / the ray cone's radius at the hit) + the bias, the page under the hit's uv -
// and the CPU maps and captures that page a frame later (MeshCardScene::setFeedback). One hit in a tile of the dither's
// side reports per frame (ditherCoord: the reader's pixel or trace texel; the tile's reporting place moves every frame).
// The report is an insert into a hash table of (element + 1, hits) pairs: element = card | level << 20 | page x << 24 |
// page y << 28; linear probing of at most CL_FEEDBACK_PROBES slots, the slot claimed by a compare-exchange. An insert
// that finds no slot counts in the header (the CPU reports it: McStats::feedbackDropped) and is lost for the frame.
// The page the hit read is also stamped with the update (last used: its lighting is refreshed first).
void clFeedback(McFrame f, uint cardIndex, McCard card, float2 localXy, uint page, float sampleRadius, uint2 ditherCoord)
{
    const uint mask = f.feedbackDither >> 16;
    if (any((ditherCoord & mask) != uint2(f.feedbackDither & 0xFFu, (f.feedbackDither >> 8) & 0xFFu))) return;
    if (f.lastUsed != MC_NONE)
    {
        RWByteAddressBuffer lastUsed = ResourceDescriptorHeap[f.lastUsed];
        lastUsed.Store(page * 4, f.frame);
    }
    if (f.feedback == MC_NONE || cardIndex >= CL_FEEDBACK_CARDS) return;
    const float resolution = max(card.extent.x, card.extent.y) / max(sampleRadius, 0.01);
    const uint level = (uint)clamp(log2(max(resolution, 1.0)) + f.feedbackBias, (float)MC_MIN_RES_LEVEL, (float)MC_MAX_RES_LEVEL);
    // the level's pages along the card's sides (MeshCardScene::mipMapDesc: the shorter side loses levels; a level of
    // more than a physical page along either side is whole pages along both)
    const int2 levelXy = clamp(int2(level, level) - int2((card.packed >> 4) & 0xFu, (card.packed >> 8) & 0xFu), (int)MC_MIN_RES_LEVEL, (int)MC_MAX_RES_LEVEL);
    uint2 pages = uint2(1, 1);
    if (any(levelXy > (int)MC_SUB_ALLOC_RES_LEVEL)) pages = 1u << (uint2(max(levelXy, (int)MC_SUB_ALLOC_RES_LEVEL)) - MC_SUB_ALLOC_RES_LEVEL);
    const float2 uv = saturate(localXy / card.extent.xy * 0.5 + 0.5);
    const uint2 pageCoord = min(uint2(uv * float2(pages)), pages - 1);
    const uint element = cardIndex | (level << 20) | (pageCoord.x << 24) | (pageCoord.y << 28);
    RWByteAddressBuffer table = ResourceDescriptorHeap[f.feedback];
    const uint first = clHash(element);
    bool stored = false;
    for (uint step = 0; step < CL_FEEDBACK_PROBES && !stored; ++step)
    {
        const uint at = CL_FEEDBACK_HEAD + ((first + step) & (CL_FEEDBACK_SLOTS - 1u)) * 8u;
        uint before;
        table.InterlockedCompareExchange(at, 0u, element + 1u, before);
        if (before == 0u || before == element + 1u)
        {
            table.InterlockedAdd(at + 4u, 1u);
            stored = true;
        }
    }
    if (!stored) table.InterlockedAdd(0u, 1u);
}

// hiRes (the reference's HighResPages sampling - its reflections and radiosity; the other readers take the resident
// level, AlwaysResidentPagesWithoutFeedback): the cards' highest level under the hit, and the hit's feedback.
// sampleRadius: the ray cone's radius at the hit (m); ditherCoord: clFeedback.
ClSample clReadCardsAt(McFrame f, uint sceneInstance, float3 position, float3 normal, uint what, bool hiRes, float sampleRadius, uint2 ditherCoord)
{
    ClSample o;
    o.valid = false;
    o.direct = o.indirect = o.final = o.albedo = o.emission = 0;
    const uint index = mcMeshCardsOf(f, sceneInstance);
    if (index == MC_NONE) return o;
    const McMeshCards mesh = mcLoadMeshCards(f, index);
    const float3 n = float3(dot(mesh.worldToLocal[0].xyz, normal), dot(mesh.worldToLocal[1].xyz, normal), dot(mesh.worldToLocal[2].xyz, normal));
    const float3 axisWeights = n * n;
    uint mask = 0;
    if (axisWeights.x > 0) mask |= mesh.cardLookup[n.x < 0 ? 0 : 1];
    if (axisWeights.y > 0) mask |= mesh.cardLookup[n.y < 0 ? 2 : 3];
    if (axisWeights.z > 0) mask |= mesh.cardLookup[n.z < 0 ? 4 : 5];
    const float bias = f.depthBias + ((mesh.countFlags & 0x20000u) != 0 ? 0.5 : 0.0);  // (mostly two-sided: hits are less reliable)
    Texture2D<float> depthAtlas = ResourceDescriptorHeap[f.depth];
    float weightSum = 0;
    // the card that weighs most in the read: the one the feedback speaks for
    float bestWeight = 0;
    uint bestCard = 0, bestPage = 0;
    float2 bestLocal = 0;
    [loop] while (mask != 0)  // (at most 32 cards: the lookup is a 32-bit mask)
    {
        const uint bit = firstbitlow(mask);
        mask ^= 1u << bit;
        const McCard card = mcLoadCard(f, mesh.cardOffset + bit);
        if (!mcVisible(card)) continue;
        const float3 local = mcWorldToCard(card, position);
        if (any(abs(local) > card.extent + 0.5 * bias)) continue;
        const McCardSample cs = mcCardSample(f, card, local.xy, hiRes);
        if (!cs.valid) continue;
        const float hitDepth = mcCardDepth(card, local);
        const float threshold = bias / card.extent.z, falloff = 0.25 * threshold;
        const int3 at = int3(cs.texel00, 0);
        const float4 depths = float4(depthAtlas.Load(at), depthAtlas.Load(at, int2(1, 0)), depthAtlas.Load(at, int2(0, 1)), depthAtlas.Load(at, int2(1, 1)));
        float4 w = 1 - saturate((abs(hitDepth - depths) - threshold) / falloff);
        w = select(depths < MC_DEPTH_NONE, w, float4(0, 0, 0, 0)) * cs.weights * axisWeights[mcDirection(card) >> 1];
        const float sum = w.x + w.y + w.z + w.w;
        if (!(sum > 0)) continue;
        weightSum += sum;
        if (sum > bestWeight)
        {
            bestWeight = sum;
            bestCard = mesh.cardOffset + bit;
            bestPage = cs.page;
            bestLocal = local.xy;
        }
        if (what & CL_READ_IRRADIANCE)
        {
            Texture2D<float3> direct = ResourceDescriptorHeap[f.directLighting];
            Texture2D<float3> indirect = ResourceDescriptorHeap[f.indirectLighting];
            o.direct += w.x * direct.Load(at) + w.y * direct.Load(at, int2(1, 0)) + w.z * direct.Load(at, int2(0, 1)) + w.w * direct.Load(at, int2(1, 1));
            o.indirect += w.x * indirect.Load(at) + w.y * indirect.Load(at, int2(1, 0)) + w.z * indirect.Load(at, int2(0, 1)) + w.w * indirect.Load(at, int2(1, 1));
        }
        if (what & CL_READ_FINAL)
        {
            Texture2D<float3> final = ResourceDescriptorHeap[f.finalLighting];
            o.final += w.x * final.Load(at) + w.y * final.Load(at, int2(1, 0)) + w.z * final.Load(at, int2(0, 1)) + w.w * final.Load(at, int2(1, 1));
        }
        if (what & CL_READ_MATERIAL)
        {
            Texture2D<float4> albedo = ResourceDescriptorHeap[f.albedo];
            Texture2D<float3> emissive = ResourceDescriptorHeap[f.emissive];
            o.albedo += w.x * mcDecodeAlbedo(albedo.Load(at).rgb) + w.y * mcDecodeAlbedo(albedo.Load(at, int2(1, 0)).rgb) + w.z * mcDecodeAlbedo(albedo.Load(at, int2(0, 1)).rgb) +
                        w.w * mcDecodeAlbedo(albedo.Load(at, int2(1, 1)).rgb);
            o.emission += w.x * emissive.Load(at) + w.y * emissive.Load(at, int2(1, 0)) + w.z * emissive.Load(at, int2(0, 1)) + w.w * emissive.Load(at, int2(1, 1));
        }
    }
    if (!(weightSum > 0)) return o;
    if (hiRes && weightSum > 0.1) clFeedback(f, bestCard, mcLoadCard(f, bestCard), bestLocal, bestPage, sampleRadius, ditherCoord);
    const float k = 1 / weightSum;
    o.valid = true;
    o.direct *= k / CL_IRRADIANCE_SCALE;
    o.indirect *= k / CL_IRRADIANCE_SCALE;
    o.final *= k / CL_RADIANCE_SCALE;
    o.albedo *= k;
    o.emission *= k / MC_EMISSIVE_SCALE;
    return o;
}
// The resident level, no feedback.
ClSample clReadCards(McFrame f, uint sceneInstance, float3 position, float3 normal, uint what)
{
    return clReadCardsAt(f, sceneInstance, position, normal, what, false, 0, uint2(0, 0));
}
// The highest level, with feedback.
ClSample clReadCardsHiRes(McFrame f, uint sceneInstance, float3 position, float3 normal, uint what, float sampleRadius, uint2 ditherCoord)
{
    return clReadCardsAt(f, sceneInstance, position, normal, what, true, sampleRadius, ditherCoord);
}

#endif
