// Screen-probe final gather (gi.lumen): the structure of Unreal's Lumen screen probe gather, written anew for this
// renderer (no code taken over; structure, numbers and algorithms from reading ue6-main's Renderer/Private/Lumen and
// Shaders/Private/Lumen; the list of deliberate differences is in Docs/Status/LUMEN_GATHER_KO.md).
//
// Probes: one per 16 x 16 px tile of the main view at a pixel that moves inside the tile from frame to frame (8-frame
// Hammersley sequence), plus adaptive probes (at most half as many again) where a pixel's 4 tile probes do not lie on
// its plane. A probe has its own row in the probe atlas (W_p wide): uniform probes first (tile order), adaptive ones
// after. Per probe: linear depth (< 0: no surface), world normal, world position, world speed.
// Probe directions: the whole sphere through the equal-area octahedral mapping (Clarberg), 8 x 8 texels, world space.
//
// Common constants (every Lg kernel): P[8] = { view width, view height, probe view width W_p, probe view height H_p },
// P[9] = { atlas rows in use (H_p + adaptive rows), tile size (16), frame bits (temporal index 0-7 | ray direction
// index << 8 | history valid << 16), frame index }, P[10] = { uniform probes, max adaptive probes, adaptive buffer
// (SRV or UAV, per pass), probe depth texture }, P[11] = { probe normal texture, probe position texture, 0, 0 }.
// Adaptive buffer (raw): [0] count, [16 + 4 i] probe i's screen position (x | y << 16), then per tile a header (its
// adaptive probe count) and LG_TILE_ADAPTIVE indices.
#ifndef UNX_GI_LUMEN_COMMON_HLSLI
#define UNX_GI_LUMEN_COMMON_HLSLI
#include "Passes/Common/Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Common/BlueNoise.hlsli"

#define LG_PI 3.14159265358979
#define LG_TRACE_RES 8u            // traces per probe: 8 x 8
#define LG_GATHER_RES 8u           // probe radiance texels after the traces are gathered
#define LG_IRRADIANCE_RES 6u       // irradiance map per probe (+ 1 texel border: 8 x 8)
#define LG_TILE_ADAPTIVE 8u        // adaptive candidates per tile (4 x 2 samples)
#define LG_MIN_INTERPOLATION_WEIGHT 0.01  // (MIN_PROBE_INTERPOLATION_WEIGHT)
#define LG_MAX_HIT_DISTANCE 65000.0

uint2 lgViewSize() { return P[8].xy; }
uint2 lgProbeViewSize() { return P[8].zw; }
uint lgAtlasRows() { return P[9].x; }
uint lgTile() { return P[9].y; }
uint lgTemporalIndex() { return P[9].z & 0xFFu; }
uint lgRayIndex() { return (P[9].z >> 8) & 0xFFu; }
bool lgHistoryValid() { return (P[9].z & 0x10000u) != 0; }
uint lgFrame() { return P[9].w; }
uint lgUniformProbes() { return P[10].x; }
uint lgMaxAdaptive() { return P[10].y; }

uint lgHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
float lgUnit(uint x) { return (lgHash(x) >> 8) * (1.0 / 16777216.0); }
// Two numbers in [0, 1) for a 2D coordinate and an index: the blue-noise tile (BlueNoise.hlsli) - blue over the
// coordinates at every index, low-discrepancy over successive indices at one coordinate (the reference reads its
// blue-noise table for the same draws: ray texel centres, pixel jitter, probe choice, dithers).
float2 lgNoise2(uint2 coord, uint index) { return blueNoise2(coord, index); }
float lgNoise1(uint2 coord, uint index) { return lgNoise2(coord, index).x; }

uint lgReverseBits(uint v)
{
    v = (v << 16) | (v >> 16);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);
    return v;
}
// Hammersley point i of n with a 16-bit scramble.
float2 lgHammersley(uint i, uint n, uint2 scramble)
{
    const float e1 = frac((float)i / n + (scramble.x & 0xFFFFu) / 65536.0);
    const float e2 = (float)((lgReverseBits(i) >> 16) ^ (scramble.y & 0xFFFFu)) / 65536.0;
    return float2(e1, e2);
}
// The tile probes' pixel inside their tile this frame, in [0, tile).
uint2 lgTileJitter(uint temporalIndex) { return uint2(lgHammersley(temporalIndex, 8, uint2(0, 0)) * lgTile()); }

// ---- the sphere <-> unit square, equal area (Clarberg 2008)
float3 lgSphere(float2 uv)
{
    uv = 2 * uv - 1;
    const float d = 1 - (abs(uv.x) + abs(uv.y));
    const float r = 1 - abs(d);
    const float phi = r == 0 ? 0 : (LG_PI / 4) * ((abs(uv.y) - abs(uv.x)) / r + 1);
    const float f = r * sqrt(2 - r * r);
    return float3(f * (uv.x < 0 ? -1.0 : 1.0) * cos(phi), f * (uv.y < 0 ? -1.0 : 1.0) * sin(phi), (d < 0 ? -1.0 : 1.0) * (1 - r * r));
}
float2 lgSphereInverse(float3 dir)
{
    const float3 a = abs(dir);
    const float r = sqrt(saturate(1 - a.z));
    const float hi = max(a.x, a.y), lo = min(a.x, a.y);
    float phi = hi == 0 ? 0 : atan2(lo, hi) * (2 / LG_PI);
    if (a.x < a.y) phi = 1 - phi;
    float2 uv = float2(r - phi * r, phi * r);
    if (dir.z < 0) uv = 1 - uv.yx;
    uv *= float2(dir.x < 0 ? -1.0 : 1.0, dir.y < 0 ? -1.0 : 1.0);
    return uv * 0.5 + 0.5;
}
// Texel of an octahedral map of side 'res' with a border of 'border' texels around it: the interior texel a border
// texel repeats (coord in [0, res + 2 border)).
uint2 lgOctWrap(int2 coord, int res, int border)
{
    int2 c = coord - border;
    if (c.x < 0 || c.x >= res)
    {
        c.x = c.x < 0 ? -1 - c.x : 2 * res - 1 - c.x;
        c.y = res - 1 - c.y;
    }
    if (c.y < 0 || c.y >= res)
    {
        c.y = c.y < 0 ? -1 - c.y : 2 * res - 1 - c.y;
        c.x = res - 1 - c.x;
    }
    return (uint2)clamp(c, 0, res - 1);
}

// ---- spherical harmonics, 3 bands (real basis, 9 coefficients)
struct LgSh
{
    float4 a;  // Y00, Y1-1, Y10, Y11
    float4 b;  // Y2-2, Y2-1, Y20, Y21
    float c;   // Y22
};
LgSh lgShBasis(float3 d)
{
    LgSh s;
    s.a = float4(0.282095, -0.488603 * d.y, 0.488603 * d.z, -0.488603 * d.x);
    s.b = float4(1.092548 * d.x * d.y, -1.092548 * d.y * d.z, 0.315392 * (3 * d.z * d.z - 1), -1.092548 * d.x * d.z);
    s.c = 0.546274 * (d.x * d.x - d.y * d.y);
    return s;
}
LgSh lgShScale(LgSh s, float k)
{
    s.a *= k;
    s.b *= k;
    s.c *= k;
    return s;
}
LgSh lgShAdd(LgSh x, LgSh y)
{
    x.a += y.a;
    x.b += y.b;
    x.c += y.c;
    return x;
}
float lgShDot(LgSh x, LgSh y) { return dot(x.a, y.a) + dot(x.b, y.b) + x.c * y.c; }
// The clamped cosine lobe around n as SH (transfer function of a diffuse surface): band factors pi, 2 pi / 3, pi / 4.
LgSh lgShCosineLobe(float3 n)
{
    LgSh s = lgShBasis(n);
    s.a *= float4(LG_PI, 2 * LG_PI / 3, 2 * LG_PI / 3, 2 * LG_PI / 3);
    s.b *= LG_PI / 4;
    s.c *= LG_PI / 4;
    return s;
}

// ---- probes
uint lgProbeCount(ByteAddressBuffer adaptive) { return lgUniformProbes() + min(adaptive.Load(0), lgMaxAdaptive()); }
uint lgProbeCount(RWByteAddressBuffer adaptive) { return lgUniformProbes() + min(adaptive.Load(0), lgMaxAdaptive()); }
uint2 lgAtlasCoord(uint probe) { return uint2(probe % lgProbeViewSize().x, probe / lgProbeViewSize().x); }
uint lgProbeIndex(uint2 atlasCoord) { return atlasCoord.y * lgProbeViewSize().x + atlasCoord.x; }
uint lgAdaptiveTileBase() { return 16 + lgMaxAdaptive() * 4; }
uint lgTileHeaderAddress(uint2 tile) { return lgAdaptiveTileBase() + (tile.y * lgProbeViewSize().x + tile.x) * 4 * (1 + LG_TILE_ADAPTIVE); }
uint2 lgUnpackScreen(uint v) { return uint2(v & 0xFFFFu, v >> 16); }
uint lgPackScreen(uint2 p) { return (p.x & 0xFFFFu) | (p.y << 16); }
// The pixel of a tile's uniform probe.
uint2 lgUniformProbePixel(uint2 tile) { return min(tile * lgTile() + lgTileJitter(lgTemporalIndex()), lgViewSize() - 1); }
// The tile a pixel belongs to under this frame's jitter (the tile whose probe is at or before the pixel).
uint2 lgTileOfPixel(uint2 pixel)
{
    const int2 p = (int2)pixel - (int2)lgTileJitter(lgTemporalIndex());
    return (uint2)clamp(p / (int)lgTile(), 0, (int2)lgProbeViewSize() - 1);
}
template <typename B>
uint2 lgProbePixel(B adaptive, uint probe)
{
    if (probe < lgUniformProbes()) return lgUniformProbePixel(lgAtlasCoord(probe));
    return lgUnpackScreen(adaptive.Load(16 + (probe - lgUniformProbes()) * 4));
}
// World position of a pixel centre at a linear view depth (the camera ray through it scaled to that depth).
float3 lgWorldAtDepth(float2 pixel, float viewDepth)
{
    const float2 ndc = float2((pixel.x + 0.5) / g_viewWidth * 2 - 1, 1 - (pixel.y + 0.5) / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, g_nearPlane / max(viewDepth, 1e-6), 1));
    return p.xyz / p.w;
}

// Probe normal: octahedral, 2 x unorm16.
float2 lgEncodeNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : (1 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
float3 lgDecodeNormal(float2 e)
{
    e = e * 2 - 1;
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}

// A trace's word: hit distance (float bits without the two low mantissa bits and the sign), bit 0 hit, bit 1 the hit
// moves relative to the probe, bit 31 the ray stopped where the radiance cache takes over.
uint lgEncodeTrace(float distance, bool hit, bool moving, bool reachedCache)
{
    return (asuint(max(distance, 0.0)) & 0x7FFFFFFCu) | (hit ? 1u : 0u) | (moving ? 2u : 0u) | (reachedCache ? 0x80000000u : 0u);
}
float lgTraceDistance(uint w) { return asfloat(w & 0x7FFFFFFCu); }
bool lgTraceHit(uint w) { return (w & 1u) != 0; }
bool lgTraceMoving(uint w) { return (w & 2u) != 0; }
// Hit distance for the probe filter, unorm: sqrt scale, 0 = no valid sample in the texel.
float lgEncodeHitDistance(float d) { return d < 0 ? 0.0 : sqrt(saturate(d / LG_MAX_HIT_DISTANCE)) * (254.0 / 255.0) + 1.0 / 255.0; }
float lgDecodeHitDistance(float e)
{
    if (e < 0.5 / 255.0) return -1;
    const float s = (e - 1.0 / 255.0) * (255.0 / 254.0);
    return s * s * LG_MAX_HIT_DISTANCE;
}
// A ray of the structured importance sampling: texel in the octahedral map of side LG_TRACE_RES << (maxLevel - level).
uint lgPackRay(uint2 texel, uint level) { return (texel.x & 0x3Fu) | ((texel.y & 0x3Fu) << 6) | ((level & 0xFu) << 12); }
void lgUnpackRay(uint v, out uint2 texel, out uint level)
{
    texel = uint2(v & 0x3Fu, (v >> 6) & 0x3Fu);
    level = (v >> 12) & 0xFu;
}
// The point inside the direction texels of a tile's probes this frame.
float2 lgTexelCentre(uint2 tile) { return lgNoise2(tile, lgRayIndex()); }
#endif
