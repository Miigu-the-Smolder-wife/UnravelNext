// Far-field radiance cache of the screen-probe final gather (lumen.radiance_cache; owner A, for R's gi.lumen). The
// structure and default numbers follow Unreal Engine's Lumen radiance cache (ue6-main LumenRadianceCache*.cpp / .usf /
// .ush, LumenRadianceCacheInterpolation.ush, read on 2026-10-01); the code is ours.
// World-space probes on camera-centred clipmaps (4 levels of 48^3 cells; level 0 spans +-25 m, each level twice the
// last): a screen-probe ray traces only to the cache's coverage distance (2 cell diagonals) and a miss there reads the
// incident radiance from the cache in the ray's direction, interpolated between the 8 probes around the ray's origin.
//   mark      a consumer marks the 8 probe cells around each position it will query (LumenRadianceCacheMark.hlsli; the
//             module's own marker marks the screen's surface at the probe spacing);
//   update    last frame's probes that are still marked (or were used within 8 frames) keep their slot through the
//             clipmaps' scrolling, the rest are freed; newly marked cells take slots; probes are ranked by
//             (frames since traced) / (clipmap + 1) into 16 log buckets (new probes first) and traced within a budget of
//             100 probes x 1024 rays (new probes always, at a quarter of the rays when over the budget);
//   trace     32 x 32 directions per probe (equal-area octahedral map), rays from the probe's centre starting one cell
//             diagonal out; hit lighting as the screen probes' rays (the surface cache, else the world cache, sun and a
//             light sample); radiance and hit distance per texel;
//   filter    each traced texel averaged with the 6 neighbour probes' texels that see the same thing (hit-angle
//             weight, mutual occlusion test), then stored with a 1-texel border for bilinear lookups.
// Probe radiance is stored as nits x LRC_RADIANCE_SCALE in RGBA16F (exposure-independent: probes outlive exposure
// changes), alpha = the texel holds radiance, rgb multiplied by it (a bilinear lookup is then weighted by validity).
// Probe occlusion (lumen.radiance_cache_probe_occlusion; ours - the reference's radiance probes have none and leak
// through walls nearer than a cell, its irradiance-field probes test Chebyshev depth): a probe's ray starts lrcTMin
// from its centre, so near a wall it starts BEYOND the wall and brings the other side's light (the sky into a closed
// room). Two tests close a closed room:
//   (i)  the trace first walks the centre's straight line to the ray's start; anything in the way (not a two-sided
//        sheet) and the texel holds nothing - its depth is the blocker's distance (< lrcTMin: lrcDepthNear);
//   (ii) a lookup uses a probe only when the probe's depth map sees the asking ray's start (lrcProbeSees): a probe on
//        the other side of a wall does not.
// A probe that passes both has its centre and its ray's start in the asking point's own space. Where no probe answers
// the lookup's weight is 0 and the caller traces its ray to the full length instead.
// Irradiance (lumen.hit_indirect_radiance_cache; the reference's CalculateIrradiance, IrradianceProbeResolution 6): every
// traced probe also keeps the irradiance of its radiance on a 6 x 6 equal-area map of normals (+ 1 border), the texels
// that hold nothing left out and the rest scaled up for them; lrcIrradiance reads it at a point - the indirect light
// of a ray hit that has no card (LumenHitIndirect.hlsli). Such hits also ask for probes where they fall (lrcHitMark: a
// list of positions the next frame's marking takes), so that a surface only a mirror shows has probes around it.
#ifndef UNX_LUMEN_RADIANCE_CACHE_HLSLI
#define UNX_LUMEN_RADIANCE_CACHE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define LRC_MAX_CLIPMAPS 6u
#define LRC_INVALID 0xFFFFFFFFu
#define LRC_USED 0xFFFFFFFEu
#define LRC_NEVER_TRACED 0u
#define LRC_HISTOGRAM 16u
#define LRC_RADIANCE_SCALE (1.0 / 64.0)
#define LRC_PI 3.14159265358979
#define LRC_IRRADIANCE_RES 6u          // a probe's irradiance map, texels a side (stored with a 1-texel border: 8)
#define LRC_IRRADIANCE_BORDERED 8u
#define LRC_IRRADIANCE_SCALE (1.0 / 64.0)  // stored: lux x this
#define LRC_HIT_MARKS 2048u            // positions the hits' mark list holds a frame (16 B each after a 16 B header)

// The frame's parameters (a raw buffer; FrameResources::lumenRcParams).
struct LrcParams
{
    float4 cornerCell[LRC_MAX_CLIPMAPS];      // world position of the clipmap's minimum corner, cell size (m)
    float4 prevCornerCell[LRC_MAX_CLIPMAPS];  // the previous frame's
    uint clipmaps, grid, probeResolution, atlasProbes;  // levels, cells per axis, texels per probe side, probes per atlas side
    float reprojectionRadiusScale, invFadeSize, tMinScale, traceDistance;
    uint frame, maxProbes, budget, keepFrames;
    float downsampleDistance;
    uint traceCapacity, tempProbes, finalResolution;  // probes traced per frame at most, the temporary atlas' probes per side, texels per probe side with its border
};
LrcParams lrcParams(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    return b.Load<LrcParams>(0);
}

float lrcCellSize(LrcParams p, uint clipmap) { return p.cornerCell[clipmap].w; }
// Probe rays start this far from the probe (one cell diagonal): what lies nearer is the screen probes' own rays'.
float lrcTMin(LrcParams p, uint clipmap) { return p.cornerCell[clipmap].w * 1.7320508 * p.tMinScale; }
float3 lrcCoordFloat(LrcParams p, float3 worldPosition, uint clipmap) { return (worldPosition - p.cornerCell[clipmap].xyz) / p.cornerCell[clipmap].w; }
float3 lrcProbePosition(LrcParams p, uint3 coord, uint clipmap) { return p.cornerCell[clipmap].xyz + (float3(coord) + 0.5) * p.cornerCell[clipmap].w; }

// The finest clipmap whose interior holds the position (its edge cells fade to the next level by the dither value);
// p.clipmaps when none does.
uint lrcClipmap(LrcParams p, float3 worldPosition, float dither)
{
    for (uint c = 0; c < p.clipmaps; ++c)
    {
        const float3 f = lrcCoordFloat(p, worldPosition, c);
        const float3 lo = saturate((f - 0.5) * p.invFadeSize), hi = saturate((float(p.grid) - 0.5 - f) * p.invFadeSize);
        if (min(min(lo.x, min(lo.y, lo.z)), min(hi.x, min(hi.y, hi.z))) > dither) return c;
    }
    return p.clipmaps;
}

// Indirection: one word per cell (clipmaps side by side along x): LRC_INVALID, or the probe's atlas index.
uint lrcIndirection(Texture3D<uint> indirection, LrcParams p, int3 coord, uint clipmap)
{
    if (any(coord < 0) || any(coord >= int(p.grid))) return LRC_INVALID;
    return indirection.Load(int4(coord.x + int(clipmap * p.grid), coord.y, coord.z, 0));
}

// Equal-area octahedral map of the sphere (Clarberg 2008): [0, 1]^2 <-> unit directions, every texel the same solid angle.
float3 lrcUvToDirection(float2 uv)
{
    const float2 u = uv * 2 - 1, a = abs(u);
    const float signedDistance = 1 - (a.x + a.y), r = 1 - abs(signedDistance);
    const float phi = (r == 0 ? 1.0 : (a.y - a.x) / r + 1) * (LRC_PI / 4);
    const float f = r * sqrt(max(2 - r * r, 0.0));
    return float3((u.x < 0 ? -1.0 : 1.0) * cos(phi) * f, (u.y < 0 ? -1.0 : 1.0) * sin(phi) * f, (signedDistance < 0 ? -1.0 : 1.0) * (1 - r * r));
}
float2 lrcDirectionToUv(float3 d)
{
    d = normalize(d);
    const float3 a = abs(d);
    const float r = sqrt(saturate(1 - a.z));
    const float hi = max(a.x, a.y), lo = min(a.x, a.y);
    float phi = atan(hi == 0 ? 0.0 : lo / hi) * (2 / LRC_PI);
    if (a.x < a.y) phi = 1 - phi;
    float v = phi * r, u = r - v;
    if (d.z < 0)
    {
        const float t = u;
        u = 1 - v;
        v = 1 - t;
    }
    return float2(d.x < 0 ? -u : u, d.y < 0 ? -v : v) * 0.5 + 0.5;
}

// Probe depth texel (R16_UINT): half-float distance without its lowest bit, bit 15 = a front face was hit, bit 0 = the
// hit material is two-sided; 0xFFFF = miss.
uint lrcEncodeDepth(float distance, bool hit, bool frontFace, bool twoSided)
{
    if (!hit) return 0xFFFFu;
    return (f32tof16(clamp(distance, 0.0, 65000.0)) & 0x7FFEu) | (frontFace ? 0x8000u : 0u) | (twoSided ? 1u : 0u);
}
float lrcDepthDistance(uint e) { return e == 0xFFFFu ? 65000.0 : f16tof32(e & 0x7FFEu); }
// The depth of a texel whose ray was not traced: the probe's straight line to the ray's start is blocked at 'distance'
// (kept under the start distance so that it reads as near: lrcDepthNear).
uint lrcEncodeNearDepth(float distance, float tMin, bool frontFace) { return lrcEncodeDepth(min(distance, tMin * 0.95), true, frontFace, false); }
bool lrcDepthHit(uint e) { return e != 0xFFFFu; }
bool lrcDepthSeesFront(uint e) { return e == 0xFFFFu || (e & 0x8001u) != 0; }

uint2 lrcAtlasCoord(LrcParams p, uint probe) { return uint2(probe % p.atlasProbes, probe / p.atlasProbes); }
bool lrcDepthNear(LrcParams p, uint clipmap, uint e) { return e != 0xFFFFu && f16tof32(e & 0x7FFEu) < lrcTMin(p, clipmap) * 0.98; }
// Whether a probe sees a point around it (within about two cells): its depth map toward the point against the distance,
// less 0.15 cell (a texel of the map is 5.6 degrees wide: 0.1 cell at a cell's distance).
bool lrcProbeSees(LrcParams p, Texture2D<uint> depth, uint probe, float3 centre, uint clipmap, float3 position)
{
    const float3 to = position - centre;
    const float distance = length(to);
    if (distance < 1e-3) return true;
    const uint2 texel = min(uint2(lrcDirectionToUv(to) * float(p.probeResolution)), p.probeResolution - 1);
    return lrcDepthDistance(depth.Load(int3(lrcAtlasCoord(p, probe) * p.probeResolution + texel, 0))) >= distance - 0.15 * lrcCellSize(p, clipmap);
}

// ---- consumers ------------------------------------------------------------------------------------------------------
struct LrcCoverage
{
    bool valid;
    uint clipmap;
    float minTraceDistance;  // a ray from the position must be traced this far before the cache may answer for the rest
};
// The cache's coverage of a position that was marked this frame (not checked: see lrcCoverageChecked).
LrcCoverage lrcCoverage(LrcParams p, float3 worldPosition, float dither)
{
    LrcCoverage c;
    c.clipmap = lrcClipmap(p, worldPosition, dither);
    c.valid = c.clipmap < p.clipmaps;
    c.minTraceDistance = c.valid ? lrcTMin(p, c.clipmap) + lrcCellSize(p, c.clipmap) * 1.7320508 : 1e7;
    return c;
}
// The same, valid only when all 8 probes around the position exist and have been traced (positions that may not have been
// marked: the caller keeps tracing to its full length otherwise).
LrcCoverage lrcCoverageChecked(LrcParams p, uint indirectionSrv, float3 worldPosition, float dither)
{
    LrcCoverage c = lrcCoverage(p, worldPosition, dither);
    if (!c.valid) return c;
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    const int3 corner = int3(floor(lrcCoordFloat(p, worldPosition, c.clipmap) - 0.5));
    for (uint i = 0; i < 8; ++i)
        if (lrcIndirection(indirection, p, corner + int3(i & 1, (i >> 1) & 1, i >> 2), c.clipmap) >= LRC_USED) c.valid = false;
    if (!c.valid) c.minTraceDistance = 1e7;
    return c;
}

// One probe's radiance toward 'direction' as seen from 'worldPosition' (rgb: nits x a; a: how much of the lookup's
// texels hold radiance): the direction is taken to the sphere of radius reprojectionRadiusScale x TMin around the probe
// (the parallax of what the probe saw beyond its start distance), and the value rescaled by the squared distance ratio
// so that moving between probes shows no grid pattern.
float4 lrcProbeRadiance(LrcParams p, Texture2D<float4> atlas, uint probe, float3 centre, uint clipmap, float3 worldPosition, float3 direction)
{
    const float radius = p.reprojectionRadiusScale * lrcTMin(p, clipmap);
    // the far intersection of the ray with the sphere (the position is inside or near it)
    const float3 oc = worldPosition - centre;
    const float b = dot(oc, direction), cc = dot(oc, oc) - radius * radius;
    const float t = -b + sqrt(max(b * b - cc, 0.0));
    const float3 reprojected = oc + direction * t;
    const float correction = t * t / max(radius * dot(reprojected, direction), 1e-6);
    const float2 uv = (float2(lrcAtlasCoord(p, probe)) * p.finalResolution + 1 + lrcDirectionToUv(reprojected) * p.probeResolution) /
                      float(p.atlasProbes * p.finalResolution);
    const float4 v = atlas.SampleLevel(g_linearClamp, uv, 0);
    return float4(v.rgb * (correction / LRC_RADIANCE_SCALE), v.a);
}

// The distance (m) at which the cache saw something from a covered position toward 'direction': the depth texel of the
// probe whose cell holds the position, at least the coverage distance. What a ray that ended in the cache reports as
// its hit distance (the screen probes' filter compares neighbours' hit distances).
float lrcSampleDistance(LrcParams p, uint indirectionSrv, uint depthSrv, LrcCoverage coverage, float3 worldPosition, float3 direction)
{
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    const int3 coord = int3(floor(lrcCoordFloat(p, worldPosition, coverage.clipmap)));
    const uint probe = lrcIndirection(indirection, p, coord, coverage.clipmap);
    if (probe >= LRC_USED) return coverage.minTraceDistance;
    Texture2D<uint> depth = ResourceDescriptorHeap[depthSrv];
    const uint2 texel = min(uint2(lrcDirectionToUv(direction) * float(p.probeResolution)), p.probeResolution - 1);
    return max(lrcDepthDistance(depth.Load(int3(lrcAtlasCoord(p, probe) * p.probeResolution + texel, 0))), coverage.minTraceDistance);
}

// The cache's incident radiance at a covered position toward 'direction': rgb nits, a = the weight that answered (of the
// 8 probes' trilinear weights: those whose depth map sees 'seenFrom' - a point of the asking ray near its start - times
// how much of their texels hold radiance). a = 0 (under 2 %): no probe answers, rgb = 0 - the caller traces on.
// depthSrv UNX_NONE: no visibility test.
#define LRC_MIN_ANSWER 0.02
float4 lrcSample(LrcParams p, uint indirectionSrv, uint atlasSrv, uint depthSrv, LrcCoverage coverage, float3 worldPosition, float3 direction, float3 seenFrom)
{
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    Texture2D<float4> atlas = ResourceDescriptorHeap[atlasSrv];
    const float3 f = lrcCoordFloat(p, worldPosition, coverage.clipmap) - 0.5;
    const int3 corner = int3(floor(f));
    const float3 a = f - floor(f);
    float4 sum = 0;
    for (uint i = 0; i < 8; ++i)
    {
        const int3 o = int3(i & 1, (i >> 1) & 1, i >> 2);
        const float w = (o.x ? a.x : 1 - a.x) * (o.y ? a.y : 1 - a.y) * (o.z ? a.z : 1 - a.z);
        if (!(w > 0)) continue;
        const uint probe = lrcIndirection(indirection, p, corner + o, coverage.clipmap);
        if (probe >= LRC_USED) continue;
        const float3 centre = lrcProbePosition(p, uint3(corner + o), coverage.clipmap);
        if (depthSrv != 0xFFFFFFFFu)
        {
            Texture2D<uint> depth = ResourceDescriptorHeap[depthSrv];
            if (!lrcProbeSees(p, depth, probe, centre, coverage.clipmap, seenFrom)) continue;
        }
        sum += lrcProbeRadiance(p, atlas, probe, centre, coverage.clipmap, worldPosition, direction) * w;
    }
    return sum.a >= LRC_MIN_ANSWER ? float4(sum.rgb / sum.a, sum.a) : float4(0, 0, 0, 0);
}
// The point of a ray the probes must see (lrcSample seenFrom): a quarter cell along it (off the surface it starts on).
float3 lrcSeenFrom(LrcParams p, LrcCoverage coverage, float3 origin, float3 direction) { return origin + direction * (0.25 * lrcCellSize(p, coverage.clipmap)); }

// The cache's irradiance (rgb lux, a = the weight that answered; 0: none) on a surface of 'normal' at a position that
// need not have been marked: the 8 probes around it in the finest clipmap where enough of them exist, have been traced
// and see the point (lrcProbeSees: the point lifted a tenth of a cell off its surface) - the point's own level first,
// then coarser ones, at most LRC_MAX_CLIPMAPS levels of 8 probes. Each probe's irradiance map bilinear at the normal,
// weighted by how much of that hemisphere the probe holds.
float4 lrcIrradiance(LrcParams p, uint indirectionSrv, uint irradianceSrv, uint depthSrv, float3 worldPosition, float3 normal)
{
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    Texture2D<float4> atlas = ResourceDescriptorHeap[irradianceSrv];
    const float2 octa = lrcDirectionToUv(normal) * float(LRC_IRRADIANCE_RES) + 1.0;
    const float side = float(p.atlasProbes * LRC_IRRADIANCE_BORDERED);
    for (uint clipmap = lrcClipmap(p, worldPosition, 0.5); clipmap < p.clipmaps; ++clipmap)
    {
        const float3 f = lrcCoordFloat(p, worldPosition, clipmap) - 0.5;
        const int3 corner = int3(floor(f));
        const float3 a = f - floor(f);
        const float3 seenFrom = worldPosition + normal * (0.1 * lrcCellSize(p, clipmap));
        float4 sum = 0;
        for (uint i = 0; i < 8; ++i)
        {
            const int3 o = int3(i & 1, (i >> 1) & 1, i >> 2);
            const float w = (o.x ? a.x : 1 - a.x) * (o.y ? a.y : 1 - a.y) * (o.z ? a.z : 1 - a.z);
            if (!(w > 0)) continue;
            const uint probe = lrcIndirection(indirection, p, corner + o, clipmap);
            if (probe >= LRC_USED) continue;
            const float3 centre = lrcProbePosition(p, uint3(corner + o), clipmap);
            if (depthSrv != 0xFFFFFFFFu)
            {
                Texture2D<uint> depth = ResourceDescriptorHeap[depthSrv];
                if (!lrcProbeSees(p, depth, probe, centre, clipmap, seenFrom)) continue;
            }
            const float4 v = atlas.SampleLevel(g_linearClamp, (float2(lrcAtlasCoord(p, probe)) * float(LRC_IRRADIANCE_BORDERED) + octa) / side, 0);
            sum += float4(v.rgb * v.a, v.a) * w;
        }
        if (sum.a >= LRC_MIN_ANSWER) return float4(sum.rgb / (sum.a * LRC_IRRADIANCE_SCALE), sum.a);
    }
    return float4(0, 0, 0, 0);
}

// A hit asks for probes around its position (the list: word 0 = positions asked for this frame - those past
// LRC_HIT_MARKS are not stored, a later frame's hits ask again - then 16 B a position from byte 16). The next frame's
// r.gi.rc.hitmark marks them between the cache's clear and its update.
void lrcHitMark(uint marksUav, float3 worldPosition)
{
    RWByteAddressBuffer marks = ResourceDescriptorHeap[marksUav];
    uint index;
    marks.InterlockedAdd(0, 1u, index);
    if (index < LRC_HIT_MARKS) marks.Store3(16 + index * 16, asuint(worldPosition));
}
#endif
