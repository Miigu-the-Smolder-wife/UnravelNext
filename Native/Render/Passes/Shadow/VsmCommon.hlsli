// Virtual shadow maps (ARCHITECTURE 2.3, 2.4, 2.11): data layout and helpers shared by the S track's kernels and its
// public lookups (ShadowVisibility.hlsli). Owner: S.
//
// Sun: a clipmap of VSM_LEVELS orthographic levels around the camera, level k covering 2^(k+4) m with VSM_VIRTUAL
// texels (texel tau_k = 2^(k-10) m), cut into VSM_TABLE x VSM_TABLE virtual pages of VSM_PAGE^2 texels. A level's window
// is [origin, origin + VSM_TABLE) pages in absolute page coordinates of its light-space grid; a page lives in table
// slot (absolute page mod VSM_TABLE) and carries its wrap generation (absolute page div VSM_TABLE) as a tag, so the
// window scrolls without moving cached pages.
//
// Stored values are light-space heights h = dot(p, towardSun) of the surface nearest the sun, as order-preserving
// uints (vsmEncode) so rasterisation resolves them with InterlockedMax; 0 = no caster.
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

struct VsmLevel
{
    int2 origin;    // absolute page coordinate of the window's first page
    float texel;    // tau_k (m)
    float pad;
};

// Per-frame constants (VsmSystem.cpp mirrors this layout): a constant buffer view of the frame's slot of an upload
// ring, read as ConstantBuffer<VsmConstants> (dynamic level indexing is a native constant load; the layout follows the
// cbuffer packing rules: every float3 is followed by a scalar, VsmLevel is 16 B).
struct VsmConstants
{
    float3 lightX;
    float hMin;              // light-space height range of the casters (raster depth mapping)
    float3 lightY;
    float hMax;
    float3 lightZ;           // towards the sun
    float tanSunRadius;
    uint poolPagesX, poolPagesY, frame, sceneInvalidate;  // sceneInvalidate: every resident page is stale
    float time;
    float lodBias;
    float receiverBiasTexels;   // tolerance above the receiver's plane, in texels of the compared level (x (1 + slope))
    float maxReceiverSlope;     // clamp of the receiver plane's light-space gradient (grazing surfaces)
    uint cacheFrames;        // unrequested pages are released after this many frames
    uint instanceCount;
    uint windTexels;         // wind moves a caster "beyond a texel" at windTexels texels (normally 1)
    uint windChanged;        // wind speed or direction changed: pages holding wind casters are stale
    float2 cameraUV;         // camera in light space: the level windows derive from it (vsmOrigin)
    uint searchTaps;         // shadow.vsm.search_taps / filter_taps: the estimator's tap counts for every caller
    uint filterTaps;
    VsmLevel level[VSM_LEVELS];  // CPU copy of the windows (raster views); kernels use vsmTexel / vsmOrigin
};

// Physical page metadata (32 B).
struct VsmPageMeta
{
    uint owner;          // slot | 0x80000000 while allocated
    uint lastRequested;  // frame
    uint renderTime;     // float bits: scene time of the last render (wind rule)
    uint maxHeight;      // encoded height of the highest caster in the page (blocker search bound)
    uint windAmplitude;  // float bits: largest wind displacement bound (windOffsetBound x scale) of the casters drawn
    uint windCaster;     // 1 when a wind-affected caster was drawn into the page
    uint pad0, pad1;
};


uint vsmEncode(float h)
{
    const uint u = asuint(h);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}
float vsmDecode(uint e) { return asfloat((e & 0x80000000u) ? (e & 0x7FFFFFFFu) : ~e); }
#define VSM_EMPTY 0u

// Level geometry by arithmetic (a per-pixel level index into the constant buffer would serialise divergent waves):
// texel 2^(k-10) m and page 2^(k-3) m as exact powers of two; window origin = floor(camera / page) - VSM_TABLE / 2,
// the same float operations as VsmSystem.cpp.
float vsmTexel(uint k) { return asfloat((117u + k) << 23); }
float vsmPageSize(uint k) { return asfloat((124u + k) << 23); }
int2 vsmOrigin(ConstantBuffer<VsmConstants> c, uint k) { return int2(floor(c.cameraUV / vsmPageSize(k))) - (int)(VSM_TABLE / 2); }

float3 vsmLightSpace(ConstantBuffer<VsmConstants> c, float3 world) { return float3(dot(world, c.lightX), dot(world, c.lightY), dot(world, c.lightZ)); }

// Finest level whose texel is not larger than the receiver's pixel footprint (ARCHITECTURE 2.3: page texel <= pixel).
uint vsmLevelForFootprint(ConstantBuffer<VsmConstants> c, float footprint)
{
    const float k = floor(log2(max(footprint, 1e-30) * 1024.0) + c.lodBias);  // tau_0 = 2^-10 m
    return (uint)clamp(k, 0.0, float(VSM_LEVELS - 1));
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

// Physical pool: a raw buffer (no texture layouts to transition between the raster's writes and the lookups' reads),
// pages of VSM_PAGE^2 texels, row-major inside a page. Byte address of texel 'local' of physical page 'phys'.
uint vsmPoolAddress(uint phys, uint2 local) { return ((phys << 14) + local.y * VSM_PAGE + local.x) * 4; }

#endif
