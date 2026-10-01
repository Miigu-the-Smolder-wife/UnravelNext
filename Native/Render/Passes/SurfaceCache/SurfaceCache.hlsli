// The surface cache: lighting of the surfaces around the camera kept in world space, independent of the view direction
// (surface_cache.*; recorded by ReflectionSystem). It plays the part of Unreal's Lumen surface cache: a ray hit reads the
// light already there instead of estimating it with one light sample. The rules and numbers follow the shipping card
// surface cache of ue6-main (LumenSceneRendering.cpp, LumenSceneLighting.cpp, LumenSceneDirectLighting.cpp,
// LumenRadiosity.cpp read as a reference; no code taken). What differs is the representation: this engine has no card
// capture, so the 'texels' are cells of a hashed world grid (the layout of ue6-main's LumenRef world cache) and they are
// found by rays instead of by rasterising cards.
//   texel     a cell: a cube of the world split by the surface's dominant axis (six faces). Its size follows the card
//             rule 'texels per card = 100 x extent / distance, at most 0.2 texels per cm': size = distance / 100, at least
//             5 cm, taken at powers of two (5 cm x 2^level; level = floor(log2(distance / 5 m)), 0 within 10 m).
//   capture   SurfaceCacheSeed: every frame rays leave the camera position uniformly over the sphere (no view direction)
//             and bounce on; each hit marks its cell (created when new; the frame's first mark sets its point, normal,
//             albedo and emission). Reflection and GI hits and the radiosity rays mark too (the reference's feedback).
//             A cell stays while it was marked within the last 255 frames (the reference keeps unused pages 256 frames).
//             When the camera moves, a point's level changes: marks then go to the cell of the new level, and until that
//             one is lit a read takes the cell the point had one level finer or coarser (the reference resamples a
//             card's lighting when it is reallocated at another resolution); the old cell ages out unmarked.
//   direct    SurfaceCacheCells, capacity / 32 cells a frame (new cells first, then a window that walks the lit ones):
//             the 8 lights with the largest unshadowed irradiance on the cell, each evaluated with one shadow ray, and
//             the sun with one. The value replaces the cell's (no time accumulation, as the reference's default).
//   radiosity probes on cells four times as large (one per 4 x 4 texels): SurfaceCacheProbes, capacity / 64 / 16 probes a
//             frame, 4 x 4 cosine-stratified rays each; a ray reads the final lighting of the cell it meets (capped at
//             40 in exposed units) or the sky; a running mean over at most 4 updates. A cell takes its indirect
//             irradiance from the 3 x 3 probes around it, weighted by distance and by their plane, when it is updated.
//   read      scRead: a cell that has been lit gives direct (local lights), sun and indirect irradiance, its albedo and
//             emission. Final lighting = (direct + sun + indirect) x albedo / pi + emission.
// One raw buffer (RW): header (SC_HEADER bytes); cells, per slot of N: keys 4 B, heads 4 B, data 16 B (position xyz,
// normal oct 16 + 16), material 8 B (albedo rgb8 sqrt, emission R11G11B10 x SC_STORE_SCALE), light 16 B (direct, sun,
// indirect as R11G11B10 x SC_STORE_SCALE, 0), list 4 B; then probes, per slot of N / 4: keys 4 B, heads 4 B, data 16 B,
// light 8 B (irradiance R11G11B10 x SC_STORE_SCALE, 0), list 4 B.
// heads: bit 0 marked since the last upkeep, bit 1 has data, bits 8-15 lighting updates (cells: 0 = not lit yet, else 1;
//        probes: frames in the running mean), bits 16-23 frames since the last mark, bits 24-31 (cells) frames in the
//        stochastic direct light's running mean (surface_cache.direct_stochastic).
// lists (written by SurfaceCacheUpdate): the lit entries from index 0 up, the entries not lit yet from the last index down.
// Feedback (the reference's r.LumenScene.Lighting.Feedback): a cell a consumer's hit marked is listed apart and half of
// the relighting budget left after the new cells goes to those cells first (SurfaceCacheLight.hlsl scPickCell).
// Header words: 13 = lit cells with feedback. 0 N, 1 frame, 2 lit cells, 3 new cells, 4-6 camera xyz (float), 7 max unused frames, 8 lit probes, 9 new
// probes, 10 flags (bit 0: marking on), 11 asuint(radiosity ray cap, exposed), 12 asuint(probe max frames).
#ifndef UNX_SURFACE_CACHE_HLSLI
#define UNX_SURFACE_CACHE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define SC_HEADER 64u
#define SC_NONE 0xFFFFFFFFu
#define SC_PROBES_STEPS 8u  // (the reference probes 4; at Lumen's texel density a room fills the table to 0.5-0.9 and 4 steps then fail often)
#define SC_STORE_SCALE (1.0 / 64.0)
#define SC_HEAD_MARKED 1u
#define SC_HEAD_VALID 2u
#define SC_HEAD_FEEDBACK 4u  // marked by a consumer's hit (a reflection or GI ray read it) since the last upkeep
#define SC_CELL_BYTES 56u
#define SC_PROBE_BYTES 36u
#define SC_TEXEL_CM 5.0          // the finest cell (the reference's 0.2 texels per cm)
#define SC_TEXEL_DISTANCE 100.0  // cell size = distance / this (the reference's card texel density scale)
#define SC_PROBE_SPACING 4.0     // a probe per this many texels along an axis

struct ScLayout
{
    uint entries;
    float3 camera;
};
uint scKeysOffset(uint i) { return SC_HEADER + i * 4; }
uint scHeadsOffset(uint n, uint i) { return SC_HEADER + n * 4 + i * 4; }
uint scDataOffset(uint n, uint i) { return SC_HEADER + n * 8 + i * 16; }
uint scMaterialOffset(uint n, uint i) { return SC_HEADER + n * 24 + i * 8; }
uint scLightOffset(uint n, uint i) { return SC_HEADER + n * 32 + i * 16; }
uint scListOffset(uint n, uint i) { return SC_HEADER + n * 48 + i * 4; }
uint scFeedbackListOffset(uint n, uint i) { return SC_HEADER + n * 52 + i * 4; }  // the lit cells consumers read (header word 13: count)
uint scProbeBase(uint n) { return SC_HEADER + n * SC_CELL_BYTES; }
uint scProbeCount(uint n) { return n / 4; }
uint scProbeKeysOffset(uint n, uint i) { return scProbeBase(n) + i * 4; }
uint scProbeHeadsOffset(uint n, uint i) { return scProbeBase(n) + scProbeCount(n) * 4 + i * 4; }
uint scProbeDataOffset(uint n, uint i) { return scProbeBase(n) + scProbeCount(n) * 8 + i * 16; }
uint scProbeLightOffset(uint n, uint i) { return scProbeBase(n) + scProbeCount(n) * 24 + i * 8; }
uint scProbeListOffset(uint n, uint i) { return scProbeBase(n) + scProbeCount(n) * 32 + i * 4; }

uint scHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint scFace(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x > 0 ? 0u : 1u;
    if (a.y >= a.z) return n.y > 0 ? 2u : 3u;
    return n.z > 0 ? 4u : 5u;
}

// The level of a point: cells of 5 cm x 2^level; a cell is distance / 100 across, never under 5 cm.
uint scLevel(ScLayout l, float3 position)
{
    const float size = distance(position, l.camera) / SC_TEXEL_DISTANCE;  // metres
    return (uint)clamp(floor(log2(max(size / (SC_TEXEL_CM * 0.01), 1.0))), 0.0, 30.0);
}
float scLevelSize(uint level) { return SC_TEXEL_CM * 0.01 * exp2((float)level); }
float scCellSize(ScLayout l, float3 position) { return scLevelSize(scLevel(l, position)); }

// Key of the cell (spacing = 1) or the probe (spacing = SC_PROBE_SPACING) at a grid coordinate: a 32-bit fingerprint of
// (coordinate, level, face), never 0 (0 is a free slot). A level's shell is up to 400 cells across, so the coordinates
// are not packed (8 bits each would alias inside the shell); two different cells share a fingerprint and a probe
// window with probability about 4 / 2^32 per lookup.
uint scKeyAt(uint level, int3 coord, uint face)
{
    const uint h = scHash((uint)coord.x * 73856093u ^ scHash((uint)coord.y * 19349663u ^ scHash((uint)coord.z * 83492791u ^ (level * 8u + face + 0x9E3779B9u))));
    return h == 0u ? 1u : h;
}
int3 scCoord(uint level, float3 position, float spacing) { return int3(floor((position + 1e-4) / (scLevelSize(level) * spacing))); }
uint scKey(ScLayout l, float3 position, float3 normal, float spacing)
{
    const uint level = scLevel(l, position);
    return scKeyAt(level, scCoord(level, position, spacing), scFace(normal));
}

uint scPackOct(float3 n)
{
    const float3 a = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    float2 o = a.xy;
    if (a.z < 0) o = (1 - abs(a.yx)) * float2(a.x >= 0 ? 1 : -1, a.y >= 0 ? 1 : -1);
    const uint2 q = uint2(round(saturate(o * 0.5 + 0.5) * 65535.0));
    return q.x | (q.y << 16);
}
float3 scUnpackOct(uint v)
{
    const float2 o = float2(v & 0xFFFFu, v >> 16) / 65535.0 * 2 - 1;
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}
// A colour (nits or lux) in 32 bits: R11G11B10 floats of the value x SC_STORE_SCALE.
uint scPackRgb(float3 c)
{
    const float3 s = clamp(c * SC_STORE_SCALE, 0.0, 64000.0);
    const uint r = min((f32tof16(s.r) + 8u) >> 4, 0x7BFu), g = min((f32tof16(s.g) + 8u) >> 4, 0x7BFu), b = min((f32tof16(s.b) + 16u) >> 5, 0x3DFu);
    return r | (g << 11) | (b << 22);
}
float3 scUnpackRgb(uint v) { return float3(f16tof32((v & 0x7FFu) << 4), f16tof32(((v >> 11) & 0x7FFu) << 4), f16tof32((v >> 22) << 5)) / SC_STORE_SCALE; }
uint scPackAlbedo(float3 a)
{
    const uint3 q = uint3(round(sqrt(saturate(a)) * 255.0));
    return q.x | (q.y << 8) | (q.z << 16);
}
float3 scUnpackAlbedo(uint v)
{
    const float3 s = float3(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu) / 255.0;
    return s * s;
}

ScLayout scLayout(RWByteAddressBuffer b)
{
    ScLayout l;
    l.entries = b.Load(0);
    l.camera = asfloat(b.Load3(16));
    return l;
}

// The slot of a key in a table of 'count' slots whose keys start at 'base'; SC_NONE when it is not there. Every probe
// step is looked at: slots are freed out of order.
uint scFind(RWByteAddressBuffer b, uint base, uint count, uint key)
{
    uint at = scHash(key) & (count - 1);
    for (uint step = 0; step < SC_PROBES_STEPS; ++step)
    {
        if (b.Load(base + at * 4) == key) return at;
        at = (at + 1) & (count - 1);
    }
    return SC_NONE;
}
// The slot of a key, taking a free slot when the table does not hold it; SC_NONE when its steps are all taken.
uint scFindOrInsert(RWByteAddressBuffer b, uint base, uint count, uint key)
{
    uint slot = scFind(b, base, count, key);
    if (slot != SC_NONE) return slot;
    uint at = scHash(key) & (count - 1);
    for (uint step = 0; step < SC_PROBES_STEPS; ++step)
    {
        uint previous;
        b.InterlockedCompareExchange(base + at * 4, 0u, key, previous);
        if (previous == 0u || previous == key) return at;
        at = (at + 1) & (count - 1);
    }
    return SC_NONE;
}

struct ScSample
{
    bool valid;                // the cell exists and has been lit
    float3 direct, sun, indirect;  // irradiance: local lights, the sun, the radiosity
    float3 albedo, emission;
};

ScSample scRead(RWByteAddressBuffer b, ScLayout l, float3 position, float3 normal)
{
    ScSample s;
    s.valid = false;
    s.direct = s.sun = s.indirect = s.albedo = s.emission = 0;
    const uint n = l.entries;
    if (n == 0) return s;
    // the point's own level, else the level it had before the camera moved (one finer, one coarser)
    const uint level = scLevel(l, position);
    const uint face = scFace(normal);
    uint slot = SC_NONE;
    for (uint attempt = 0; attempt < 3 && slot == SC_NONE; ++attempt)
    {
        if (attempt == 1 && level == 0) continue;
        const uint at = attempt == 0 ? level : attempt == 1 ? level - 1 : level + 1;
        const uint found = scFind(b, scKeysOffset(0), n, scKeyAt(at, scCoord(at, position, 1.0), face));
        if (found == SC_NONE) continue;
        const uint head = b.Load(scHeadsOffset(n, found));
        if ((head & SC_HEAD_VALID) != 0 && ((head >> 8) & 0xFFu) != 0) slot = found;
    }
    if (slot == SC_NONE) return s;
    s.valid = true;
    const uint3 light = b.Load3(scLightOffset(n, slot));
    s.direct = scUnpackRgb(light.x);
    s.sun = scUnpackRgb(light.y);
    s.indirect = scUnpackRgb(light.z);
    const uint2 material = b.Load2(scMaterialOffset(n, slot));
    s.albedo = scUnpackAlbedo(material.x);
    s.emission = scUnpackRgb(material.y);
    return s;
}
// What leaves a lit cell toward any direction (a diffuse surface): the final lighting.
float3 scFinalLighting(ScSample s) { return (s.direct + s.sun + s.indirect) * s.albedo / 3.14159265 + s.emission; }

// Marks the cell of a surface point and its probe (created when new); the frame's first mark sets the point and the
// material. Returns the cell's slot, SC_NONE when its steps are all taken by other cells or marking is off.
uint scMarkAs(RWByteAddressBuffer b, ScLayout l, float3 position, float3 normal, float3 albedo, float3 emission, uint bits);
// A consumer's mark (a reflection or GI hit that reads the cell): the cell is in use and its lighting is wanted soon.
uint scMark(RWByteAddressBuffer b, ScLayout l, float3 position, float3 normal, float3 albedo, float3 emission)
{
    return scMarkAs(b, l, position, normal, albedo, emission, SC_HEAD_MARKED | SC_HEAD_FEEDBACK);
}
// The cache's own rays (capture, radiosity): the cell exists and stays, with no claim on the relighting order.
uint scMarkQuiet(RWByteAddressBuffer b, ScLayout l, float3 position, float3 normal, float3 albedo, float3 emission)
{
    return scMarkAs(b, l, position, normal, albedo, emission, SC_HEAD_MARKED);
}
uint scMarkAs(RWByteAddressBuffer b, ScLayout l, float3 position, float3 normal, float3 albedo, float3 emission, uint bits)
{
    const uint n = l.entries;
    if (n == 0 || (b.Load(40) & 1u) == 0) return SC_NONE;
    const uint level = scLevel(l, position);
    const uint face = scFace(normal);
    const uint slot = scFindOrInsert(b, scKeysOffset(0), n, scKeyAt(level, scCoord(level, position, 1.0), face));
    if (slot == SC_NONE) return SC_NONE;
    uint before;
    b.InterlockedOr(scHeadsOffset(n, slot), bits, before);
    if ((before & SC_HEAD_MARKED) == 0)
    {
        b.Store4(scDataOffset(n, slot), uint4(asuint(position), scPackOct(normal)));
        b.Store2(scMaterialOffset(n, slot), uint2(scPackAlbedo(albedo), scPackRgb(emission)));
    }
    const uint probes = scProbeCount(n);
    const uint probe = scFindOrInsert(b, scProbeKeysOffset(n, 0), probes, scKeyAt(level, scCoord(level, position, SC_PROBE_SPACING), face));
    if (probe != SC_NONE)
    {
        b.InterlockedOr(scProbeHeadsOffset(n, probe), SC_HEAD_MARKED, before);
        if ((before & SC_HEAD_MARKED) == 0) b.Store4(scProbeDataOffset(n, probe), uint4(asuint(position), scPackOct(normal)));
    }
    return slot;
}

#endif
