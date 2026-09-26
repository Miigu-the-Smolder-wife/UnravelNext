// Virtual shadow maps (ARCHITECTURE 2.3, 2.4, 2.11): data layout and helpers shared by the S track's kernels and its
// public lookups (ShadowVisibility.hlsli). Owner: S.
//
// Sun: a clipmap of VSM_LEVELS orthographic levels around the camera, level k covering 2^(k+4) m with VSM_VIRTUAL
// texels (texel tau_k = 2^(k-10) m), cut into VSM_TABLE x VSM_TABLE virtual pages of VSM_PAGE^2 texels. A level's window
// is [origin, origin + VSM_TABLE) pages in absolute page coordinates of its light-space grid; a page lives in table
// slot (absolute page mod VSM_TABLE) and carries its wrap generation (absolute page div VSM_TABLE) as a tag.
//
// One path (S request 20260926_S_vsm_one_path): no page cache. Every frame the requested pages get physical pages by a
// deterministic scan (VsmScan), the depth atlas is cleared and V rasterises every requested page with hardware depth,
// all levels on the current sun. Stored in the atlas: sun pages v = (h - hMin) / (hMax - hMin) of the surface nearest the
// sun (h = dot(p, towardSun), hMin / hMax the frame's caster range), local pages the reversed-Z face depth; 0 = no
// caster. Lookups turn a texel into an order-preserving uint key (vsmEncode of h; of -z for local faces), 0 = empty.
#ifndef UNX_VSM_COMMON_HLSLI
#define UNX_VSM_COMMON_HLSLI
#include "Bindless.hlsli"

#define VSM_LEVELS 20u  // texel 1 mm .. 512 m: pixels and air to 64 km get their own level (not the 2 m level clamped)
#define VSM_PAGE 128u
#define VSM_PAGE_SHIFT 7u
#define VSM_TABLE 128u          // virtual pages per level axis (VSM_VIRTUAL / VSM_PAGE)
#define VSM_TABLE_SHIFT 7u
#define VSM_VIRTUAL 16384u
#define VSM_SLOTS_PER_LEVEL (VSM_TABLE * VSM_TABLE)
#define VSM_SUN_SLOTS (VSM_LEVELS * VSM_SLOTS_PER_LEVEL)

// Block hierarchy per physical page (VsmPageMax): VsmBlock entries for blocks of 8, 16, 32, 64 and 128 texels; level m
// has (16 >> m)^2 blocks starting at VSM_BLOCK_OFFSET(m).
struct VsmBlock
{
    uint2 range;      // min, max encoded height (min = VSM_EMPTY: the block has a texel without caster)
    float3 plane;     // least-squares plane of the casters: h - ref ~ a x + b y + c, x, y in page texels
    float2 residual;  // bounds of h - ref - plane over the block's non-empty texels
    float ref;        // the page's highest caster
};
#define VSM_BLOCK_BYTES 32u
#define VSM_BLOCK_OFFSET_8 0u
#define VSM_BLOCK_OFFSET_16 256u
#define VSM_BLOCK_OFFSET_32 320u
#define VSM_BLOCK_OFFSET_64 336u
#define VSM_BLOCK_OFFSET_128 340u
#define VSM_BLOCK_ENTRIES 341u
uint vsmBlockOffset(uint m) { return m == 0 ? 0u : m == 1 ? 256u : m == 2 ? 320u : m == 3 ? 336u : 340u; }

// Page table entry .x
#define VSM_PHYS_MASK 0x003FFFFFu
#define VSM_FLAG_DIRTY (1u << 29)      // rendered this frame
#define VSM_FLAG_STALE (1u << 30)      // content invalid: render when next requested
#define VSM_FLAG_RESIDENT (1u << 31)
// Request flags (one uint per slot)
#define VSM_REQ_PIXEL 1u
#define VSM_REQ_PROPAGATED 2u
#define VSM_REQ_AIR 4u  // requested by the air marks (VsmMarkAir; with VSM_REQ_PROPAGATED: the same page handling)

// A clipmap level (64 B). Each level keeps the sun basis its pages were rendered with (moving sun: levels refresh to the
// current sun in turn, S_STATUS_KO.md 7; the lookup error of a basis Delta theta old is <= 2 Delta theta / (pi tan
// theta_s) in visibility, kept below shadow.vsm.sun_refresh_error). Levels with the same basis id share one light-space
// grid (their texels nest), so walks to coarser levels reuse coordinates only then.
struct VsmLevel
{
    float3 lightX;   // the level's light basis (lightZ towards the sun it was rendered with)
    float cameraU;   // the camera in this basis: the window derives from it (vsmOrigin)
    float3 lightY;
    float cameraV;
    float3 lightZ;
    uint basis;      // basis id (changes when the level refreshes)
    int2 origin;     // absolute page coordinate of the window's first page (CPU copy)
    float hMin, hMax;  // light-space height range of the casters in this basis (raster depth mapping)
};

// Per-frame constants (VsmSystem.cpp mirrors this layout): a constant buffer view of the frame's slot of an upload
// ring, read as ConstantBuffer<VsmConstants> (dynamic level indexing is a native constant load; the layout follows the
// cbuffer packing rules: every float3 is followed by a scalar, VsmLevel is 64 B). lightX..Z, hMin/hMax and cameraUV
// are the current sun's (the newest basis); lookups use their level's (vsmLightSpaceAt).
struct VsmConstants
{
    float3 lightX;
    float hMin;              // light-space height range of the casters (raster depth mapping)
    float3 lightY;
    float hMax;
    float3 lightZ;           // towards the sun
    float tanSunRadius;
    uint poolPagesX, poolPagesY, frame, sceneInvalidate;  // bits 0..19: the level's pages are stale (its basis refreshed);
                                                          // bit 31: every resident page is stale (scene reload)
    float time;
    float lodBias;
    float receiverBiasTexels;   // tolerance above the receiver's plane, in texels of the compared level (x (1 + slope))
    float maxReceiverSlope;     // clamp of the receiver plane's light-space gradient (grazing surfaces)
    uint cacheFrames;        // unrequested pages are released after this many frames
    uint instanceCount;
    uint windTexels;         // wind moves a caster "beyond a texel" at windTexels texels (normally 1)
    uint windChanged;        // wind speed or direction changed this frame (statistics; the page rule uses the bound)
    float2 cameraUV;         // camera in light space: the level windows derive from it (vsmOrigin)
    uint searchTaps;         // shadow.vsm.search_taps / filter_taps: the estimator's tap counts for every caller
    uint filterTaps;
    float3 windDirection;    // this frame's scene wind (unit; zero without wind) and speed (m/s): pages store the wind
    float windSpeed;         // they were drawn with, the rule bounds the change (windChangeBound, v1.23)
    uint useStats;           // shadow.vsm.use_stats (measurement only): 1 + UAV index of the per-slot read bits, 0 = off
    uint atlasSrv;           // SRV of the page atlas (Texture2D<float>, FrameResources::vsmAtlas): every lookup reads it here
    uint fragmentCheck;      // shadow.vsm.fragment_check (verification only): ShadowFragments evaluates every record
    uint usePad;
    VsmLevel level[VSM_LEVELS];  // CPU copy of the windows (raster views); kernels use vsmTexel / vsmOrigin
};

// Physical page metadata (32 B).
struct VsmPageMeta
{
    uint owner;          // slot | 0x80000000 while allocated
    uint lastRequested;  // frame
    uint renderTime;     // float bits: scene time of the last render (wind rule)
    uint maxHeight;      // encoded height of the highest caster in the page (blocker search bound)
    uint windScale;      // float bits: largest speed-free wind bound (windOffsetScale x instance scale) of the casters drawn
    uint windCaster;     // 1 when a wind-affected caster was drawn into the page
    uint windSpeed;      // float bits: the scene wind speed the page was drawn with (v1.23)
    uint windDirection;  // octahedral snorm16 x 2 of the scene wind direction the page was drawn with
    uint layer;          // transmittance layer (revision 1, 4.2): its page + 1; 0 = no thin casters (T = 1)
    uint pad0, pad1, pad2;
};  // 48 B (VsmSystem.cpp kMetaBytes)


uint vsmEncode(float h)
{
    const uint u = asuint(h);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}
float vsmDecode(uint e) { return asfloat((e & 0x80000000u) ? (e & 0x7FFFFFFFu) : ~e); }
#define VSM_EMPTY 0u

// S error bits (INTERFACES 3.6): VSM stats word 15, OR-ed by a kernel whose data-dependent loop reached its hard cap
// (the result is then truncated, not silently accepted); any bit fails the gates (VsmStats::errorBits).
#define VSM_STATS_ERROR_BYTE 60u
#define VSM_ERR_AIR_WALK 0x1u       // vsmAirShadowFraction: a page, block or texel walk stopped at its cap
#define VSM_ERR_MARK_AIR_WALK 0x2u  // VsmMarkAir: the page walk stopped at its cap
#define VSM_ERR_MARK_FRAGMENT_WALK 0x4u  // VsmMarkFragments: the page walk stopped at its cap
#define VSM_ERR_LOCAL_AIR_WALK 0x8u  // FroxelIntegrate: a local light's air walk stopped at VSM_LOCAL_AIR_STEPS
#define VSM_ERR_MARK_LOCAL_AIR_WALK 0x10u  // VsmLocalMarkAir: the page walk stopped at VSM_LOCAL_AIR_PAGE_STEPS

// Level geometry by arithmetic (a per-pixel level index into the constant buffer would serialise divergent waves):
// texel 2^(k-10) m and page 2^(k-3) m as exact powers of two; window origin = floor(camera / page) - VSM_TABLE / 2,
// the same float operations as VsmSystem.cpp.
float vsmTexel(uint k) { return asfloat((117u + k) << 23); }
float vsmPageSize(uint k) { return asfloat((124u + k) << 23); }
int2 vsmOrigin(ConstantBuffer<VsmConstants> c, uint k) { return int2(floor(float2(c.level[k].cameraU, c.level[k].cameraV) / vsmPageSize(k))) - (int)(VSM_TABLE / 2); }

// Light space of the current sun (the newest basis; the levels' may be older): for callers that pick no level.
float3 vsmLightSpace(ConstantBuffer<VsmConstants> c, float3 world) { return float3(dot(world, c.lightX), dot(world, c.lightY), dot(world, c.lightZ)); }
// Light space of level k's basis: every lookup and mark on level k.
float3 vsmLightSpaceAt(ConstantBuffer<VsmConstants> c, float3 world, uint k)
{
    return float3(dot(world, c.level[k].lightX), dot(world, c.level[k].lightY), dot(world, c.level[k].lightZ));
}
bool vsmSameBasis(ConstantBuffer<VsmConstants> c, uint k, uint j) { return c.level[k].basis == c.level[j].basis; }

// The page of level j (coarser) over page 'page' of level k: the nested ancestor when the bases agree; otherwise the
// page holding the centre of 'page' (at level k's middle caster height) in level j's grid. heightSlack bounds how much a
// light-space height of any point of the page differs between the two bases (|h_j(q) - h_k(q)| = |q . (Z_j - Z_k)| <=
// |q| |Z_j - Z_k|): heights and bounds carried from j to k grow by it (conservative).
int2 vsmPageAcross(ConstantBuffer<VsmConstants> c, int2 page, uint k, uint j, out float heightSlack)
{
    heightSlack = 0;
    if (vsmSameBasis(c, k, j)) return page >> (int)(j - k);
    const float2 centre = (float2(page) + 0.5) * vsmPageSize(k);
    const float h = 0.5 * (c.level[k].hMin + c.level[k].hMax);
    const float3 world = c.level[k].lightX * centre.x + c.level[k].lightY * centre.y + c.level[k].lightZ * h;
    const float extent = length(world) + 2 * vsmPageSize(k) + (c.level[k].hMax - c.level[k].hMin);
    heightSlack = length(c.level[j].lightZ - c.level[k].lightZ) * extent;
    return int2(floor(vsmLightSpaceAt(c, world, j).xy / vsmPageSize(j)));
}
// Encoded height raised by 'slack' metres (VSM_EMPTY stays empty).
uint vsmRaise(uint encoded, float slack) { return (encoded == 0u || slack == 0) ? encoded : vsmEncode(vsmDecode(encoded) + slack); }

// Finest level whose texel is not larger than the receiver's pixel footprint (ARCHITECTURE 2.3: page texel <= pixel).
uint vsmLevelForFootprint(ConstantBuffer<VsmConstants> c, float footprint)
{
    const float k = floor(log2(max(footprint, 1e-30) * 1024.0) + c.lodBias);  // tau_0 = 2^-10 m
    return (uint)clamp(k, 0.0, float(VSM_LEVELS - 1));
}

// Linear view depth at which a view ray's footprint level (vsmLevelForFootprint of z x pixelScale) reaches k + 1:
// floor(log2(z pixelScale 1024) + lodBias) = k + 1 at z = 2^(k + 1 - lodBias) / (1024 pixelScale) (fragment segments).
float vsmFragmentLevelEnd(ConstantBuffer<VsmConstants> c, uint k, float pixelScale)
{
    return k + 1 < VSM_LEVELS ? exp2(float(k + 1) - c.lodBias) / (1024.0 * pixelScale) : 3.0e38;
}

// Absolute texel (integer) of a light-space position at level k; floor division keeps negative coordinates exact.
int2 vsmAbsTexel(ConstantBuffer<VsmConstants> c, float2 uv, uint k) { return int2(floor(uv / vsmTexel(k))); }
int2 vsmAbsPage(int2 absTexel) { return absTexel >> (int)VSM_PAGE_SHIFT; }  // arithmetic shift = floor division
bool vsmInWindow(ConstantBuffer<VsmConstants> c, int2 absPage, uint k) { return all(absPage >= vsmOrigin(c, k)) && all(absPage < vsmOrigin(c, k) + (int)VSM_TABLE); }
uint vsmSlot(int2 absPage, uint k) { return k * VSM_SLOTS_PER_LEVEL + (uint(absPage.y) & (VSM_TABLE - 1)) * VSM_TABLE + (uint(absPage.x) & (VSM_TABLE - 1)); }
uint vsmTag(int2 absPage) { return (uint(absPage.x >> (int)VSM_TABLE_SHIFT) & 0xFFFFu) | (uint(absPage.y >> (int)VSM_TABLE_SHIFT) << 16); }

// Absolute page held by a slot of level k under the level's current window.
int2 vsmSlotAbsPage(ConstantBuffer<VsmConstants> c, uint slotInLevel, uint k)
{
    const int2 s = int2(slotInLevel & (VSM_TABLE - 1), slotInLevel >> VSM_TABLE_SHIFT);
    const int2 o = vsmOrigin(c, k);
    // The unique a in [o, o + TABLE) with a = s (mod TABLE).
    return o + ((s - (o & (int)(VSM_TABLE - 1))) & (int)(VSM_TABLE - 1));
}

// Physical atlas (FrameResources::vsmAtlas, D32_FLOAT): page p at pixel ((p % 128), (p / 128)) x VSM_PAGE; texel
// 'local' of page 'phys'.
#define VSM_ATLAS_PAGES_PER_ROW 128u
int3 vsmAtlasTexel(uint phys, uint2 local)
{
    return int3(((phys & (VSM_ATLAS_PAGES_PER_ROW - 1)) << VSM_PAGE_SHIFT) + local.x, ((phys >> 7) << VSM_PAGE_SHIFT) + local.y, 0);
}
// Key of a sun texel v = (h - hMin) / (hMax - hMin): vsmEncode(h), VSM_EMPTY for v = 0 (no caster).
uint vsmSunKey(float v, float hMin, float hMax) { return v > 0 ? vsmEncode(hMin + v * (hMax - hMin)) : VSM_EMPTY; }
// Key of a local face texel of reversed-Z depth d = n (f - z) / ((f - n) z): vsmEncode(-z), VSM_EMPTY for d = 0.
uint vsmLocalKeyOfDepth(float d, float nearM, float farM) { return d > 0 ? vsmEncode(-(nearM * farM / (nearM + d * (farM - nearM)))) : VSM_EMPTY; }
// The atlas SRV of this frame (VsmConstants::atlasSrv), for callers that hold ShadowSrvs.
uint vsmAtlasSrv(uint constantsCbv)
{
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[constantsCbv];
    return c.atlasSrv;
}

#endif
