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
// Probe radiance is stored as nits x LRC_RADIANCE_SCALE in R11G11B10F (exposure-independent: probes outlive exposure
// changes).
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
bool lrcDepthHit(uint e) { return e != 0xFFFFu; }
bool lrcDepthSeesFront(uint e) { return e == 0xFFFFu || (e & 0x8001u) != 0; }

uint2 lrcAtlasCoord(LrcParams p, uint probe) { return uint2(probe % p.atlasProbes, probe / p.atlasProbes); }

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

// One probe's radiance toward 'direction' as seen from 'worldPosition' (nits): the direction is taken to the sphere of
// radius reprojectionRadiusScale x TMin around the probe (the parallax of what the probe saw beyond its start distance),
// and the value rescaled by the squared distance ratio so that moving between probes shows no grid pattern.
float3 lrcProbeRadiance(LrcParams p, Texture3D<uint> indirection, Texture2D<float4> atlas, int3 coord, uint clipmap, float3 worldPosition, float3 direction)
{
    const uint probe = lrcIndirection(indirection, p, coord, clipmap);
    if (probe >= LRC_USED) return 0;
    const float3 centre = lrcProbePosition(p, uint3(coord), clipmap);
    const float radius = p.reprojectionRadiusScale * lrcTMin(p, clipmap);
    // the far intersection of the ray with the sphere (the position is inside or near it)
    const float3 oc = worldPosition - centre;
    const float b = dot(oc, direction), cc = dot(oc, oc) - radius * radius;
    const float t = -b + sqrt(max(b * b - cc, 0.0));
    const float3 reprojected = oc + direction * t;
    const float correction = t * t / max(radius * dot(reprojected, direction), 1e-6);
    const float2 uv = (float2(lrcAtlasCoord(p, probe)) * p.finalResolution + 1 + lrcDirectionToUv(reprojected) * p.probeResolution) /
                      float(p.atlasProbes * p.finalResolution);
    return atlas.SampleLevel(g_linearClamp, uv, 0).rgb * (correction / LRC_RADIANCE_SCALE);
}

// The cache's incident radiance (nits) at a covered position toward 'direction'. random in [0, 1): one of the 8 probes is
// drawn by its trilinear weight (the default, as the screen probes' many rays average it); random < 0: all 8, weighted.
float3 lrcSample(LrcParams p, uint indirectionSrv, uint atlasSrv, LrcCoverage coverage, float3 worldPosition, float3 direction, float random)
{
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    Texture2D<float4> atlas = ResourceDescriptorHeap[atlasSrv];
    const float3 f = lrcCoordFloat(p, worldPosition, coverage.clipmap) - 0.5;
    const int3 corner = int3(floor(f));
    const float3 a = f - floor(f);
    float3 sum = 0;
    float cumulative = 0;
    for (uint i = 0; i < 8; ++i)
    {
        const int3 o = int3(i & 1, (i >> 1) & 1, i >> 2);
        const float w = (o.x ? a.x : 1 - a.x) * (o.y ? a.y : 1 - a.y) * (o.z ? a.z : 1 - a.z);
        if (random < 0)
        {
            if (w > 0) sum += lrcProbeRadiance(p, indirection, atlas, corner + o, coverage.clipmap, worldPosition, direction) * w;
            continue;
        }
        cumulative += w;
        if (random < cumulative || i == 7) return lrcProbeRadiance(p, indirection, atlas, corner + o, coverage.clipmap, worldPosition, direction);
    }
    return sum;
}
#endif
