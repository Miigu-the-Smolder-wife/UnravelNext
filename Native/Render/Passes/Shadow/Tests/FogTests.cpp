// S fog (atmosphere.fog: Passes/Atmosphere/Fog.hlsli, FogVolume.hlsli, FogScatter.hlsl, FogIntegrate.hlsl; FroxelSystem.cpp
// recordFogVolume), correctness. Test stand-ins for V's raster, M's G-buffer (TestRaster.h) and V's depth pyramid
// (FogTestHiz.hlsl). The references are C++ twins of the fog's model in double precision, written from its description:
//   medium    extinction(y) = density x min(2^(-falloff (y - height)), 64), no fog nearer than the start distance along
//             the ray; a local volume adds density x fade toward its boundary x 2^(-falloff x the height inside it) with
//             its own albedo; the density's variation multiplies both;
//   light     the sun: E x the air's transmittance to the sun (FogProbe MODE 4: the GPU's own lookup at the reference's
//             points, so the fog's code is not judged by the atmosphere table's accuracy) x Henyey-Greenstein x (1 - shadow);
//   volume    per cell one sample of the medium at the frame's place in the cell (history: the blend of the frames'
//             samples), per column the slabs' integral; past the cells the closed form in the far slices.
// Storage tolerance (every comparison with the volume): its texels and the cells' are fp16, and a float written to one
// is cut toward zero (measured on this hardware: 0.729977 is stored as 0.729492, not as the nearer 0.729980) - a stored
// value is up to 2^-10 below the one written (relative), and a value past fp16's largest is stored as 65504. The
// volumes hold their light x the view's exposure (FogVolume.hlsli), so that is the precision of the exposed values:
// the test reads the volume back, divides by the frame's exposure and compares in nits. A cell's extinction low by
// 2^-10 moves the optical depth tau to a face by tau 2^-10; its source and the stored radiance add 2^-10 each:
// transmittance within 2^-10 (1 + tau) + 2e-5, radiance within 2^-10 (3 + tau) + 2e-5 (2e-5: float32 against double
// over 112 slabs). Under fp16's smallest normal value (6.1e-5: an exposed source per metre in thin fog, an extinction
// under 6.1e-5 / m, the radiance of the first slices) the cut is an absolute 6e-8 instead (6e-8 / exposure in nits, per
// metre of the path for a source; x 10 with the history): added per cell where the twin's value is that small. Two more
// absolute terms, in units of the optical depth (the radiance: x the source per unit of it): a cell's sample point is a
// float32 position - the camera's + the ray's x the depth, an ulp of its coordinates and 1e-6 of its distance off -, so
// the medium is the one that far away: what that changes of the extinction, per cell (positionSlack: it only matters
// where the density has an edge, a local volume's fade); and a slab's 1 - e^-tau in float32, 1.2e-7 per slab.
//  1. closed     fogOpticalDepth against the integral of fogExtinctionAt by Simpson's rule (pieces split at the height
//                where the density is held): rays up, down, level, across the fog's height and the hold, the start
//                distance, the cap of 64 - 1e-7 (the quadrature's own error); the GPU's float evaluation against the twin
//                at the same queries - 5e-4 of the optical depth (below the 2^-10 of the transmittance's storage);
//  2. grid       fogSliceOfDepth / fogDepthOfSlice round trip, the far slices' boundaries, fogSegment, on the CPU and the
//                GPU (float: 2e-4 slices, 4e-6 of depth + 1 / k); in a frame fogAt against the volume read back (the
//                hardware's 8 bit filter weights: 3 / 256 of the difference between the texels it blends; its result has
//                the texels' precision - measured: every value fogAt returns is an fp16 value - 2^-10), against the
//                exact medium at cell centres (plus the reader's interpolation error, which is computed and printed), and
//                across volumetric_distance_m (no step at 80 m);
//  3. uniform    falloff 0, no noise, the sun alone, no casters, no local lights, no indirect light: the volume's faces
//                against L(d) = S (1 - e^(-sigma d)), T(d) = e^(-sigma d), near and far slices, history off and on (the
//                history adds what the sun's transmittance changes over a cell: 5e-4); thin and dense media, a phase
//                function with g = 0.6 toward a sun inside the view; strong forward scattering toward the full sun, whose
//                radiance passes 65504 nits (3e5: the exposed volume holds it);
//  4. falloff    the same with height falloff for columns looking up and down: the twin at the frame's sample points
//                (storage tolerance); with the history on the blend of the frames' samples, 0.1 of each new one (plus its
//                cut in fp16 each frame, 10 x 2^-10) - in every compared column, the last row's too, whose cells' centres
//                lie on the view's edge at 1080 rows; and the closed form along the centre rays (plus the cell bound: a
//                sample anywhere in a cell is within e^(k dy) - 1 of the cell's mean extinction, dy the cell's extent in
//                height);
//  5. energy     a caster between the sun and the cells (no sun scattered: the radiance does not change across those
//                cells, to the bit); the far slices with far_shadows behind a ridge (the same, slice by slice) and
//                without; sky_amount 0, 0.5, 1 (fogOverSky); start_distance_m in the cells and in the far slices;
//                fogOverAir, fogOverRay, fogVolumeAt, the debug view; the fog off;
//  6. volumes    FrameContext::fogVolumes: an ellipsoid and a box with yaw, edge fade, height falloff and albedo, with the
//                height fog and alone (the volume runs), 18 volumes (the first 16), the origin offset (a rebased scene:
//                the same volume to the bit);
//  7. planar     a planar reflection view's volume (fogPrepareSecondary): no fog before the mirror plane;
//  8. noise      the density's variation: bounds, zero mean (3 standard errors of 2^24 lattice values and of the sample),
//                the lattice's period 256, GPU against the twin (2e-5); in a frame with the wind's drift.
//  9. rays       froxelRayAt (FroxelCommon.hlsli: the air volume's, the fog's and the sampled lights' rays) with the camera
//                at the render origin, 1,000 m and 10,000 m from it, for the main view and a cropped planar reflection
//                view: the pixel's ray at unit view depth against the camera's own basis in double precision - 0.01 pixel
//                (float32 on unit vectors gives 1e-3 pixel; 1 pixel = 2 tan(fov / 2) / height); the local volumes in a
//                frame 1,000 m from the render origin (section volumes);
// 10. exposure   the history across an exposure change (the cells are rescaled by exposure now / exposure then): the
//                same 40 frames with the last one 4 stops up (a power of two: the same bits), with the last one 2.6
//                stops up (3 x 2^-10: the last frame's cut, in the cells and in the volume), with the last 12 frames 2.6
//                stops up (the history's cut, 10 x 2^-10, + 2 x 2^-10) - each against the run without the change, in
//                nits, and against the twin.
//   unx_test_shadow_fogtests [--cpu] [--only name,name] [--no-debug-layer] [--width W --height H] [--set key=value]
//   (--cpu: the parts that need no device; names: closed grid noise uniform reader falloff start shadow far volumes
//    rebase planar rays exposure)
#include "TestRaster.h"

#include "../../Atmosphere/AtmosphereReference.h"
#include "FroxelSystem.h"
#include "VsmSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;
namespace ref = unx::render::atmosphere::reference;
using ref::D3;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kUlp = 1.0 / 1024.0;  // fp16: a stored value is up to 2^-10 below the written one (relative; cut toward zero)
constexpr double kHistoryRounding = 10.0 / 1024.0;  // history weight 0.9: each frame's cut decays by 0.9 (sum 10)
constexpr double kHalfMax = 65504.0;  // fp16's largest value: what a larger one is stored as

D3 d3(float3 v) { return { v.x, v.y, v.z }; }
double len(D3 a) { return std::sqrt(ref::dot(a, a)); }

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// ---- The medium (Fog.hlsli FogMedium, FogVolume.hlsli fogExtinctionAt).
struct Medium
{
    double density = 0, falloff = 0, height = 0, g = 0, start = 0;
    double albedo[3] = { 1, 1, 1 };
};
double extinctionAt(const Medium& m, double y) { return m.density * std::min(std::exp2(-m.falloff * (y - m.height)), 64.0); }

// The optical depth by its definition: the integral of extinctionAt along origin + dir t over [max(t0, start), t1], held
// to 64. Simpson's rule on the two pieces either side of the height where the density is held (the integrand is smooth
// on each), steps of at most 0.05 in the exponent.
double opticalDepthNumeric(const Medium& m, D3 o, D3 d, double t0, double t1)
{
    t0 = std::max(t0, m.start);
    if (!(t1 > t0) || !(m.density > 0)) return 0;
    const double k = m.falloff * std::log(2.0);
    double cut = t0;
    if (m.falloff > 0 && std::abs(d.y) > 1e-300) cut = std::clamp((m.height - 6.0 / m.falloff - o.y) / d.y, t0, t1);
    double sum = 0;
    const double ends[3] = { t0, cut, t1 };
    for (int piece = 0; piece < 2; ++piece)
    {
        const double a = ends[piece], b = ends[piece + 1];
        if (!(b > a)) continue;
        const int n = 2 * (int)std::clamp(std::ceil(k * std::abs(d.y) * (b - a) / 0.05), 32.0, 2.0e6);
        const double h = (b - a) / n;
        double s = extinctionAt(m, o.y + d.y * a) + extinctionAt(m, o.y + d.y * b);
        for (int i = 1; i < n; ++i) s += extinctionAt(m, o.y + d.y * (a + i * h)) * (i & 1 ? 4.0 : 2.0);
        sum += s * h / 3.0;
    }
    return std::min(sum, 64.0);
}

// Fog.hlsli fogOpticalDepth in double.
double opticalDepthClosed(const Medium& m, D3 o, D3 d, double t0, double t1)
{
    t0 = std::max(t0, m.start);
    if (!(t1 > t0) || !(m.density > 0)) return 0;
    const double k = m.falloff * std::log(2.0);
    const double xa = -k * (o.y + d.y * t0 - m.height), xb = -k * (o.y + d.y * t1 - m.height);
    const double lo = std::min(xa, xb), hi = std::max(xa, xb), held = std::log(64.0);
    double mean = std::min(std::exp(lo), 64.0);
    if (hi - lo > 1e-9)
    {
        const double l = std::min(lo, held), u = std::min(hi, held);
        mean = (std::exp(u) * -std::expm1(-(u - l)) + 64.0 * (std::max(hi, held) - std::max(lo, held))) / (hi - lo);
    }
    return std::min(m.density * mean * (t1 - t0), 64.0);
}

// ---- The grid (FogVolume.hlsli FogGrid), with the parameters the kernels get (floats).
struct GridCpu
{
    uint32_t x = 0, y = 0, z = 0, cellPx = 0, zFar = 0;
    double farM = 0, k = 0, b = 0, farEndM = 0;
};
GridCpu gridOf(const FogView& f) { return { f.gridX, f.gridY, f.gridZ, f.cellPx, f.farSlices, f.farM, f.k, f.b, f.farEndM }; }
double sliceOfDepth(const GridCpu& g, double depth) { return std::log2(std::max(depth, 0.0) * g.k + 1.0) * g.b; }
double depthOfSlice(const GridCpu& g, double slice) { return (std::exp2(slice / g.b) - 1.0) / g.k; }
double farDepth(const GridCpu& g, double i) { return g.farM * std::exp2(i / g.zFar * std::log2(g.farEndM / g.farM)); }
// fogAt's slice coordinate of a view depth (0 at the camera, z at farM, z + zFar at the far slices' end).
double readerCoord(const GridCpu& g, double depth)
{
    if (depth <= g.farM) return sliceOfDepth(g, depth);
    return g.zFar ? g.z + std::log2(depth / g.farM) * (g.zFar / std::log2(g.farEndM / g.farM)) : (double)g.z;
}
void segment(double& za, double& zb, double limit)
{
    const double behind = std::max(zb - limit, 0.0);
    zb -= behind;
    za = std::max(za - behind, 0.0);
}

// ---- The density's variation (FogVolume.hlsli fogLatticeValue, fogValueNoise, fogDensityScale), in the kernels' floats.
float latticeValue(int32_t cx, int32_t cy, int32_t cz)
{
    uint32_t h = ((uint32_t)cx & 255u) | ((uint32_t)cy & 255u) << 8 | ((uint32_t)cz & 255u) << 16;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
}
float mix(float a, float b, float t) { return a + (b - a) * t; }
float valueNoise(float px, float py, float pz)
{
    const float bx = std::floor(px), by = std::floor(py), bz = std::floor(pz);
    const float fx = px - bx, fy = py - by, fz = pz - bz;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy), uz = fz * fz * (3.0f - 2.0f * fz);
    const int32_t cx = (int32_t)bx, cy = (int32_t)by, cz = (int32_t)bz;
    const float x00 = mix(latticeValue(cx, cy, cz), latticeValue(cx + 1, cy, cz), ux);
    const float x10 = mix(latticeValue(cx, cy + 1, cz), latticeValue(cx + 1, cy + 1, cz), ux);
    const float x01 = mix(latticeValue(cx, cy, cz + 1), latticeValue(cx + 1, cy, cz + 1), ux);
    const float x11 = mix(latticeValue(cx, cy + 1, cz + 1), latticeValue(cx + 1, cy + 1, cz + 1), ux);
    return mix(mix(x00, x10, uy), mix(x01, x11, uy), uz);
}
float densityNoise(float lx, float ly, float lz)
{
    return (valueNoise(lx, ly, lz) + 0.5f * valueNoise(lx * 2.0f + 37.0f, ly * 2.0f + 17.0f, lz * 2.0f + 59.0f)) * (1.0f / 1.5f);
}
float densityScale(float lx, float ly, float lz, float amount)
{
    if (!(amount > 0)) return 1.0f;
    return std::max(1.0f + amount * 2.0f * densityNoise(lx, ly, lz), 0.0f);
}

// The frame's place inside the cells (FroxelSystem.cpp fogHalton: 16 frames of the Halton points 2, 3, 5).
float halton(uint32_t index, uint32_t base)
{
    float f = 1, r = 0;
    for (uint32_t i = index; i > 0; i /= base)
    {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

struct ViewCpu
{
    ViewDesc v;
    D3 cam;
    // The ray through a pixel position scaled to unit view depth (FroxelCommon.hlsli froxelRayAt: the view-space point
    // at view depth 1 from the projection's terms, turned to the world by the view matrix's rows).
    D3 rayAt(double px, double py) const
    {
        const double x = (px / v.width * 2 - 1 + (double)v.proj.m[0][2] - (double)v.proj.m[0][3]) / (double)v.proj.m[0][0];
        const double y = (1 - py / v.height * 2 + (double)v.proj.m[1][2] - (double)v.proj.m[1][3]) / (double)v.proj.m[1][1];
        return D3{ v.view.m[0][0] * x + v.view.m[1][0] * y - v.view.m[2][0], v.view.m[0][1] * x + v.view.m[1][1] * y - v.view.m[2][1],
                   v.view.m[0][2] * x + v.view.m[1][2] * y - v.view.m[2][2] };
    }
};

struct Sample
{
    double x, y, z, w;  // a frame's place inside the cells, [0, 1)^3, and its share of the cells' value
};
struct Model
{
    ViewCpu view;
    GridCpu grid;
    Medium medium;                       // heights in the frame's render space
    double noiseAmount = 0, noiseInvScale = 0;
    float noiseOffset[3] = { 0, 0, 0 };  // lattice coordinate = render position x (1, 2, 1) / scale + this
    std::vector<FogVolumeDesc> volumes;  // world; the first kMaxFogVolumes take effect
    D3 origin;                           // world = render + origin (GpuScene::originOffset)
    D3 sun;                              // unit, toward the sun
    double E[3] = { 0, 0, 0 };           // lux
    double clip[4] = { 0, 0, 0, 0 };     // a planar reflection view's plane (0: none)
    std::vector<Sample> samples;
    double exposure = 1;                 // the smallest exposure of the frames blended (the volumes hold their light x it)
    double cuts = 1;                     // fp16 cuts a cell's value carries: 1, with the history up to 10 (weight 0.9)
};
constexpr double kHalfNormal = 6.2e-5;   // under it an fp16 value is a denormal ...
constexpr double kHalfStep = 5.97e-8;    // ... with this absolute step (2^-24)

double phaseHg(double cosine, double g)
{
    const double den = 1 + g * g - 2 * g * cosine;
    return (1 - g * g) / (4 * kPi * den * std::sqrt(den));
}

// The medium at a render-space point 'along' metres from the camera on its ray: extinction and albedo.
struct Cell
{
    double sigma = 0;
    double albedo[3] = { 1, 1, 1 };
};
Cell mediumAt(const Model& m, D3 p, double along)
{
    Cell c;
    double variation = 1;
    if (m.noiseAmount > 0)
        variation = densityScale((float)(p.x * m.noiseInvScale) + m.noiseOffset[0], (float)(p.y * 2 * m.noiseInvScale) + m.noiseOffset[1],
                                 (float)(p.z * m.noiseInvScale) + m.noiseOffset[2], (float)m.noiseAmount);
    double sigma = extinctionAt(m.medium, p.y) * variation;
    if (along < m.medium.start) sigma = 0;
    double scattering[3] = { m.medium.albedo[0] * sigma, m.medium.albedo[1] * sigma, m.medium.albedo[2] * sigma };
    const size_t count = std::min<size_t>(m.volumes.size(), kMaxFogVolumes);
    for (size_t i = 0; i < count; ++i)
    {
        const FogVolumeDesc& v = m.volumes[i];
        // the volume's axes: its x along (cos yaw, 0, -sin yaw), its z along (sin yaw, 0, cos yaw) of the world
        const double dx = p.x + m.origin.x - v.centre[0], dy = p.y + m.origin.y - v.centre[1], dz = p.z + m.origin.z - v.centre[2];
        const double cy = std::cos((double)v.yaw), sy = std::sin((double)v.yaw);
        const double ux = (cy * dx - sy * dz) / v.halfSize[0], uy = dy / v.halfSize[1], uz = (sy * dx + cy * dz) / v.halfSize[2];
        const double reach = v.shape != 0 ? std::max({ std::abs(ux), std::abs(uy), std::abs(uz) }) : std::sqrt(ux * ux + uy * uy + uz * uz);
        if (reach >= 1.0) continue;
        const double fade = std::clamp((1.0 - reach) / std::clamp((double)v.edge, 1e-3, 1.0), 0.0, 1.0);
        const double s = std::max((double)v.density, 0.0) * fade * std::exp2(-std::max((double)v.heightFalloff, 0.0) * (0.5 * uy + 0.5)) * variation;
        sigma += s;
        for (int k = 0; k < 3; ++k) scattering[k] += s * v.albedo[k];
    }
    if (count > 0 && sigma > 0)
        for (int k = 0; k < 3; ++k) c.albedo[k] = scattering[k] / sigma;
    else
        for (int k = 0; k < 3; ++k) c.albedo[k] = m.medium.albedo[k];
    if ((m.clip[0] != 0 || m.clip[1] != 0 || m.clip[2] != 0 || m.clip[3] != 0) && m.clip[0] * p.x + m.clip[1] * p.y + m.clip[2] * p.z + m.clip[3] < 0) sigma = 0;
    c.sigma = sigma;
    return c;
}

// Where a view's fog starts on a ray (AtmosphereCommon.hlsli airViewStart: a planar reflection view's mirror).
double viewStart(const Model& m, D3 dir)
{
    if (m.clip[0] == 0 && m.clip[1] == 0 && m.clip[2] == 0 && m.clip[3] == 0) return 0;
    const double nl = std::sqrt(m.clip[0] * m.clip[0] + m.clip[1] * m.clip[1] + m.clip[2] * m.clip[2]);
    const double side = (m.clip[0] * m.view.cam.x + m.clip[1] * m.view.cam.y + m.clip[2] * m.view.cam.z + m.clip[3]) / nl;
    if (side >= 0) return 0;
    const double dn = (m.clip[0] * dir.x + m.clip[1] * dir.y + m.clip[2] * dir.z) / nl;
    return dn > 0 ? -side / dn : 3.0e38;
}

// A cell's sample point of one frame: on the ray through its place in the cell, at its place in the slice.
D3 samplePoint(const Model& m, uint32_t cx, uint32_t cy, uint32_t cz, const Sample& s, D3* direction = nullptr, double* along = nullptr)
{
    const D3 ray = m.view.rayAt((cx + s.x) * m.grid.cellPx, (cy + s.y) * m.grid.cellPx);
    const double zs = depthOfSlice(m.grid, cz + s.z), toRay = len(ray);
    if (direction) *direction = ray * (1.0 / toRay);
    if (along) *along = zs * toRay;
    return m.view.cam + ray * zs;
}

// The points whose sun a column's reference takes (FogProbe MODE 4), grid.z + 1 of them: per cell the frame's sample
// point (several frames: the cell's centre), then the far slices' point.
void sunPoints(const Model& m, uint32_t cx, uint32_t cy, std::vector<float4>& out)
{
    const Sample centre{ 0.5, 0.5, 0.5, 1 };
    for (uint32_t z = 0; z < m.grid.z; ++z)
    {
        const D3 p = samplePoint(m, cx, cy, z, m.samples.size() == 1 ? m.samples[0] : centre);
        out.push_back({ (float)p.x, (float)p.y, (float)p.z, 0 });
    }
    const D3 ray = m.view.rayAt((cx + 0.5) * m.grid.cellPx, (cy + 0.5) * m.grid.cellPx);
    const double toRay = len(ray);
    const D3 dir = ray * (1.0 / toRay);
    // (a ray that never reaches a planar view's mirror has no far fog: any point serves)
    const D3 p = m.view.cam + dir * std::min(std::max(m.grid.farM * toRay, viewStart(m, dir)), 1.0e7);
    out.push_back({ (float)p.x, (float)p.y, (float)p.z, 0 });
}

// The integrated volume's column: transmittance, optical depth and radiance at the far face of each slice (grid.z near
// slices, then grid.zFar far ones).
struct Column
{
    std::vector<double> T, tau;
    std::vector<std::array<double, 3>> L;
    std::vector<double> slack;               // per face: the optical depth the float32 sample positions and the denormal
                                             // extinctions may move (absolute)
    std::array<double, 3> perTau{ 0, 0, 0 };  // the column's largest source per unit of optical depth (nits)
    std::vector<std::array<double, 3>> cut;   // per face: the radiance the denormal sources' cut may move (nits, absolute)
};

// What a cell's extinction changes by over the distance its float32 sample point may be off: the camera's position + the
// ray x the depth - an ulp of the point's coordinates and 1e-6 of its distance from the camera.
double positionSlack(const Model& m, D3 p, double along, double sigma)
{
    const double reach = std::max({ std::abs(m.view.cam.x), std::abs(m.view.cam.y), std::abs(m.view.cam.z) });
    const double step = 1.2e-7 * (reach + along) + 1e-6 * along + 1e-6;
    double worst = 0;
    for (int axis = 0; axis < 3; ++axis)
        for (double side : { -step, step })
        {
            D3 q = p;
            (axis == 0 ? q.x : (axis == 1 ? q.y : q.z)) += side;
            worst = std::max(worst, std::abs(mediumAt(m, q, along).sigma - sigma));
        }
    return 1.75 * worst;  // (any direction: the three axes' changes together)
}

// The far slices of a column after its cells (FogIntegrate.hlsl): the closed form, the column's far source.
// lit: the far slices' share outside the casters' shadow (null: 1).
void farSlices(const Model& m, uint32_t cx, uint32_t cy, const float4* sun, const std::vector<double>* lit, double T, double tau, std::array<double, 3> L, Column& c)
{
    const D3 ray = m.view.rayAt((cx + 0.5) * m.grid.cellPx, (cy + 0.5) * m.grid.cellPx);
    const double toRay = len(ray);
    const D3 dir = ray * (1.0 / toRay);
    const double tStart = viewStart(m, dir), phase = phaseHg(ref::dot(dir, m.sun), m.medium.g);
    const float4 s = sun[m.grid.z];
    const double sunT[3] = { std::isfinite(s.x) ? s.x : 0.0, std::isfinite(s.y) ? s.y : 0.0, std::isfinite(s.z) ? s.z : 0.0 };
    for (uint32_t i = 0; i < m.grid.zFar; ++i)
    {
        const double za = farDepth(m.grid, i), zb = farDepth(m.grid, i + 1.0);
        const double dTau = opticalDepthClosed(m.medium, m.view.cam, dir, std::max(za * toRay, tStart), zb * toRay), t = std::exp(-dTau);
        const double share = lit ? (*lit)[m.grid.z + i] : 1.0;
        for (int k = 0; k < 3; ++k)
        {
            L[k] += T * m.medium.albedo[k] * m.E[k] * sunT[k] * phase * share * (1 - t);
            c.perTau[k] = std::max(c.perTau[k], m.medium.albedo[k] * m.E[k] * sunT[k] * phase);
        }
        T *= t;
        tau += dTau;
        c.T.push_back(T);
        c.tau.push_back(tau);
        c.L.push_back(L);
        c.slack.push_back(c.slack.empty() ? 0.0 : c.slack.back());
        c.cut.push_back(c.cut.empty() ? std::array<double, 3>{ 0, 0, 0 } : c.cut.back());
    }
}

// The twin of FogScatter.hlsl + FogIntegrate.hlsl: each cell holds the frames' samples of the medium and of the sun it
// scatters (model.samples), each slice is a homogeneous slab of the centre ray's length.
// sun: the column's sunPoints values. lit: per slice (near, then far) the share outside the casters' shadow (null: 1).
Column twinColumn(const Model& m, uint32_t cx, uint32_t cy, const float4* sun, const std::vector<double>* lit = nullptr)
{
    Column c;
    const double toRay = len(m.view.rayAt((cx + 0.5) * m.grid.cellPx, (cy + 0.5) * m.grid.cellPx));
    double T = 1, tau = 0, slack = 0;
    std::array<double, 3> L{ 0, 0, 0 }, cut{ 0, 0, 0 };
    for (uint32_t z = 0; z < m.grid.z; ++z)
    {
        const double d = (depthOfSlice(m.grid, z + 1.0) - depthOfSlice(m.grid, z)) * toRay;
        double sigma = 0, source[3] = { 0, 0, 0 };
        for (const Sample& s : m.samples)
        {
            D3 dir;
            double along = 0;
            const D3 p = samplePoint(m, cx, cy, z, s, &dir, &along);
            const Cell cell = mediumAt(m, p, along);
            const double phase = phaseHg(ref::dot(dir, m.sun), m.medium.g), share = lit ? (*lit)[z] : 1.0;
            const double sunT[3] = { sun[z].x, sun[z].y, sun[z].z };
            sigma += s.w * cell.sigma;
            slack += s.w * positionSlack(m, p, along, cell.sigma) * d;
            for (int k = 0; k < 3; ++k)
            {
                source[k] += s.w * cell.albedo[k] * m.E[k] * sunT[k] * phase * share * cell.sigma;
                c.perTau[k] = std::max(c.perTau[k], cell.albedo[k] * m.E[k] * sunT[k] * phase);
            }
        }
        // (the cell's stored values where they are fp16 denormals: an absolute cut)
        if (sigma < kHalfNormal) slack += kHalfStep * m.cuts * d;
        for (int k = 0; k < 3; ++k)
            if (source[k] * m.exposure < kHalfNormal) cut[k] += T * d * kHalfStep * m.cuts / m.exposure;
        const double t = std::exp(-sigma * d);
        for (int k = 0; k < 3; ++k) L[k] += T * (sigma > 1e-7 ? source[k] * ((1 - t) / sigma) : source[k] * d);
        T *= t;
        tau += sigma * d;
        c.T.push_back(T);
        c.tau.push_back(tau);
        c.L.push_back(L);
        c.slack.push_back(slack);
        c.cut.push_back(cut);
    }
    farSlices(m, cx, cy, sun, lit, T, tau, L, c);
    return c;
}

// The closed form along the column's centre ray (the height fog alone: no local volume, no variation): the transmittance
// at a face is e^(-the closed form's optical depth), the radiance the sum of each slice's source x what the slice removes.
Column closedColumn(const Model& m, uint32_t cx, uint32_t cy, const float4* sun)
{
    Column c;
    const D3 ray = m.view.rayAt((cx + 0.5) * m.grid.cellPx, (cy + 0.5) * m.grid.cellPx);
    const double toRay = len(ray);
    const D3 dir = ray * (1.0 / toRay);
    const double phase = phaseHg(ref::dot(dir, m.sun), m.medium.g);
    double T = 1, tau = 0, slack = 0;
    std::array<double, 3> L{ 0, 0, 0 }, cut{ 0, 0, 0 };
    for (uint32_t z = 0; z < m.grid.z; ++z)
    {
        const double before = tau, d = (depthOfSlice(m.grid, z + 1.0) - depthOfSlice(m.grid, z)) * toRay;
        tau = opticalDepthClosed(m.medium, m.view.cam, dir, 0.0, depthOfSlice(m.grid, z + 1.0) * toRay);
        const double face = std::exp(-tau), sigma = (tau - before) / d;  // (the slice's mean extinction)
        const double sunT[3] = { sun[z].x, sun[z].y, sun[z].z };
        if (sigma < kHalfNormal) slack += kHalfStep * m.cuts * d;
        for (int k = 0; k < 3; ++k)
        {
            const double perTau = m.medium.albedo[k] * m.E[k] * sunT[k] * phase;
            if (perTau * sigma * m.exposure < kHalfNormal) cut[k] += T * d * kHalfStep * m.cuts / m.exposure;
            L[k] += perTau * (T - face);
            c.perTau[k] = std::max(c.perTau[k], perTau);
        }
        T = face;
        c.T.push_back(T);
        c.tau.push_back(tau);
        c.L.push_back(L);
        c.slack.push_back(slack);
        c.cut.push_back(cut);
    }
    farSlices(m, cx, cy, sun, nullptr, T, tau, L, c);
    return c;
}

// The largest change of the medium's extinction inside the cells of a column up to each near slice, relative: a sample
// anywhere in a cell is within e^(k dy) - 1 of the cell's mean, dy the cell's extent in height (its eight corners).
std::vector<double> cellBound(const Model& m, uint32_t cx, uint32_t cy)
{
    std::vector<double> bound;
    const double k = m.medium.falloff * std::log(2.0);
    double worst = 0;
    for (uint32_t z = 0; z < m.grid.z; ++z)
    {
        double lo = 1e300, hi = -1e300;
        for (int corner = 0; corner < 8; ++corner)
        {
            const D3 p = samplePoint(m, cx, cy, z, Sample{ (double)(corner & 1), (double)((corner >> 1) & 1), (double)(corner >> 2), 1 });
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
        }
        worst = std::max(worst, std::exp(k * (hi - lo)) - 1);
        bound.push_back(worst);
    }
    return bound;
}

// The integrated volume read back (RGBA16F: rgb = radiance x the frame's exposure, a = transmittance at each slice's far
// face); at() gives the radiance in nits.
struct Volume
{
    std::vector<uint8_t> bytes;
    uint32_t x = 0, y = 0, z = 0;
    double exposure = 1;  // of the frame that wrote it
    const uint8_t* texel(uint32_t cx, uint32_t cy, uint32_t cz) const
    {
        const size_t pitch = TestFrame::rowPitch(x, 8);
        return bytes.data() + ((size_t)cz * y + cy) * pitch + (size_t)cx * 8;
    }
    std::array<double, 4> at(uint32_t cx, uint32_t cy, uint32_t cz) const
    {
        uint16_t h[4];
        std::memcpy(h, texel(cx, cy, cz), 8);
        return { halfToFloat(h[0]) / exposure, halfToFloat(h[1]) / exposure, halfToFloat(h[2]) / exposure, halfToFloat(h[3]) };
    }
    bool sameRadiance(uint32_t cx, uint32_t cy, uint32_t za, uint32_t zb) const { return std::memcmp(texel(cx, cy, za), texel(cx, cy, zb), 6) == 0; }
};

// The hardware's fetch of the volume at texel-space coordinates (coordinate x size; clamp addressing, exact weights).
// range: per channel, the spread of the eight texels blended (the 8 bit weights' error is a share of it).
std::array<double, 4> sampleVolume(const Volume& v, double tx, double ty, double tz, std::array<double, 4>* range = nullptr)
{
    auto axis = [](double t, uint32_t n, uint32_t idx[2], double& f) {
        const double s = std::clamp(t - 0.5, -1.0, (double)n);
        const double fl = std::floor(s);
        f = s - fl;
        idx[0] = (uint32_t)std::clamp(fl, 0.0, (double)n - 1);
        idx[1] = (uint32_t)std::clamp(fl + 1, 0.0, (double)n - 1);
    };
    uint32_t ix[2], iy[2], iz[2];
    double fx, fy, fz;
    axis(tx, v.x, ix, fx);
    axis(ty, v.y, iy, fy);
    axis(tz, v.z, iz, fz);
    std::array<double, 4> out{ 0, 0, 0, 0 }, lo{ 1e300, 1e300, 1e300, 1e300 }, hi{ -1e300, -1e300, -1e300, -1e300 };
    for (int c = 0; c < 8; ++c)
    {
        const int a = c & 1, b = (c >> 1) & 1, d = c >> 2;
        const double w = (a ? fx : 1 - fx) * (b ? fy : 1 - fy) * (d ? fz : 1 - fz);
        const std::array<double, 4> t = v.at(ix[a], iy[b], iz[d]);
        for (int k = 0; k < 4; ++k)
        {
            out[k] += w * t[k];
            lo[k] = std::min(lo[k], t[k]);
            hi[k] = std::max(hi[k], t[k]);
        }
    }
    if (range)
        for (int k = 0; k < 4; ++k) (*range)[k] = hi[k] - lo[k];
    return out;
}

// FogVolume.hlsli fogAt on the volume read back: uv in the view, the reader's slice coordinate, from nothing at the
// camera inside the first slice.
std::array<double, 4> readerAt(const Volume& v, const GridCpu& g, uint32_t width, uint32_t height, double u, double w, double depth, std::array<double, 4>* range = nullptr)
{
    const double c = readerCoord(g, depth);
    std::array<double, 4> s = sampleVolume(v, u * width / g.cellPx, w * height / g.cellPx, std::max(c, 1.0) - 0.5, range);
    if (c < 1.0)
    {
        for (int k = 0; k < 3; ++k) s[k] *= c;
        s[3] = 1.0 + (s[3] - 1.0) * c;
    }
    return s;
}

struct Box
{
    float3 centre, half;
};
// Distance along d (any length; in units of it) from o to the box, +inf when missed.
double hitDistance(const Box& b, D3 o, D3 d)
{
    double t0 = 0, t1 = 1e300;
    const double oc[3] = { o.x, o.y, o.z }, dc[3] = { d.x, d.y, d.z };
    const double c[3] = { b.centre.x, b.centre.y, b.centre.z }, h[3] = { b.half.x, b.half.y, b.half.z };
    for (int a = 0; a < 3; ++a)
    {
        const double lo = c[a] - h[a], hi = c[a] + h[a];
        if (std::abs(dc[a]) < 1e-15)
        {
            if (oc[a] < lo || oc[a] > hi) return INFINITY;
            continue;
        }
        double ta = (lo - oc[a]) / dc[a], tb = (hi - oc[a]) / dc[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return INFINITY;
    }
    return t0;
}
bool shadowedAt(const std::vector<Box>& casters, D3 p, D3 sun)
{
    for (const Box& b : casters)
        if (hitDistance(b, p, sun) < INFINITY) return true;
    return false;
}
// The stretch [za, zb] (view depth) of the rays through a pixel position and 'pad' pixels beside it: 1 every probed point
// is in the sun, 0 every one is in the casters' shadow, -1 both.
int litClass(const Model& m, const std::vector<Box>& casters, double px, double py, double pad, double za, double zb)
{
    int lit = 0, dark = 0;
    const double offsets[5][2] = { { 0, 0 }, { pad, 0 }, { -pad, 0 }, { 0, pad }, { 0, -pad } };
    for (const auto& o : offsets)
    {
        const D3 ray = m.view.rayAt(px + o[0], py + o[1]);
        for (int i = 0; i <= 16; ++i)
        {
            const double z = za + (zb - za) * i / 16.0;
            (shadowedAt(casters, m.view.cam + ray * z, m.sun) ? dark : lit)++;
        }
    }
    return dark == 0 ? 1 : (lit == 0 ? 0 : -1);
}
// View depth of the nearest caster surface on those rays (+inf: none).
double surfaceDepth(const Model& m, const std::vector<Box>& casters, double px, double py, double pad)
{
    double nearest = INFINITY;
    const double offsets[5][2] = { { 0, 0 }, { pad, 0 }, { -pad, 0 }, { 0, pad }, { 0, -pad } };
    for (const auto& o : offsets)
    {
        const D3 ray = m.view.rayAt(px + o[0], py + o[1]);
        for (const Box& b : casters) nearest = std::min(nearest, hitDistance(b, m.view.cam, ray));
    }
    return nearest;
}

// Comparison of a column's faces with a reference.
struct Tally
{
    double worstT = 0, worstL = 0;  // largest relative differences
    uint32_t faces = 0, over = 0, notFinite = 0;
};
// extra: per face, a relative allowance on the optical depth to it beyond the storage tolerance (null: none).
void compareFaces(const Volume& v, uint32_t cx, uint32_t cy, const Column& r, uint32_t first, uint32_t last, const std::vector<double>* extra, Tally& t, const char* label)
{
    for (uint32_t z = first; z < last; ++z)
    {
        const std::array<double, 4> g = v.at(cx, cy, z);
        const double e = extra ? (*extra)[std::min<size_t>(z, extra->size() - 1)] : 0.0;
        const double tolT = (kUlp * (1 + r.tau[z]) + 2e-5 + r.tau[z] * e + r.slack[z]) * r.T[z] + 6e-8;
        const double errT = std::abs(g[3] - r.T[z]);
        bool bad = !(errT <= tolT);
        t.worstT = std::max(t.worstT, errT / std::max(r.T[z], 1e-6));
        for (int k = 0; k < 3; ++k)
        {
            // (the texel's own cut: 2^-10 of it, or the denormals' step)
            const double tolL = (kUlp * (2 + r.tau[z]) + 2e-5 + e) * r.L[z][k] + std::max(kUlp * r.L[z][k], kHalfStep / v.exposure) + r.cut[z][k] +
                                r.perTau[k] * (r.slack[z] + 1.2e-7 * (z + 1)) + 1e-6;
            const double errL = std::abs(g[k] - r.L[z][k]);
            bad = bad || !(errL <= tolL);
            t.worstL = std::max(t.worstL, errL / std::max(r.L[z][k], 1e-3));
        }
        if (!std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2]) || !std::isfinite(g[3])) ++t.notFinite;
        ++t.faces;
        if (bad)
        {
            if (t.over < 6)
                logf("  %s: cell (%u, %u) slice %u: gpu L %.6g %.6g %.6g T %.6g | ref L %.6g %.6g %.6g T %.6g (tau %.4g)\n", label, cx, cy, z, g[0], g[1], g[2], g[3],
                     r.L[z][0], r.L[z][1], r.L[z][2], r.T[z], r.tau[z]);
            ++t.over;
        }
    }
}

std::vector<float4> floatsOf(const std::vector<uint8_t>& bytes)
{
    std::vector<float4> out(bytes.size() / 16);
    if (!out.empty()) std::memcpy(out.data(), bytes.data(), out.size() * 16);
    return out;
}
uint32_t bitsOf(float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return u;
}

// V's depth pyramid for the fog's kernels: only the level they read, made from the test raster's depth (FogTestHiz.hlsl).
struct TestHiz
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> texture;
    uint32_t uav = 0, width = 0, height = 0, mip = 0;
    ~TestHiz()
    {
        if (!device) return;
        device->descriptors().freeResource(uav);
        device->deferRelease(texture);
    }
    TextureRef record(FramePassContext& fc, TextureRef depth, D3D12_GPU_VIRTUAL_ADDRESS constants, uint32_t viewWidth, uint32_t viewHeight, uint32_t cellPx)
    {
        uint32_t level = 0;
        while ((2u << level) < cellPx) ++level;  // the mip whose texels are cellPx pixels (mip 0: 2 pixels)
        const uint32_t gridX = (viewWidth + cellPx - 1) / cellPx, gridY = (viewHeight + cellPx - 1) / cellPx;
        if (!texture || width != gridX << level || height != gridY << level || mip != level)
        {
            if (texture)
            {
                fc.device.descriptors().freeResource(uav);
                fc.device.deferRelease(texture);
                texture.Reset();
            }
            device = &fc.device;
            width = gridX << level;
            height = gridY << level;
            mip = level;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = width;
            rd.Height = height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = (UINT16)(level + 1);
            rd.Format = DXGI_FORMAT_R32_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(fc.device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&texture)),
                  "test depth pyramid");
            uav = fc.device.descriptors().allocateResource();
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = level;
            fc.device.d3d()->CreateUnorderedAccessView(texture.Get(), nullptr, &ud, fc.device.descriptors().resourceCpu(uav));
        }
        const TextureRef hiz = fc.graph.importTexture(texture.Get(), TextureDesc{ "test depth pyramid", width, height, 1, (uint16_t)(mip + 1), DXGI_FORMAT_R32_FLOAT },
                                                      D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/FogTestHiz");
        const uint32_t target = uav;
        fc.graph.addPass("s.test.hiz", QueueType::Compute,
                         [&](PassBuilder& b) {
                             b.use(depth, Use::SrvCompute);
                             b.use(hiz, Use::UavCompute);
                         },
                         [=](PassContext& ctx) {
                             const uint32_t k[4] = { ctx.srv(depth), target, cellPx, 0 };
                             ctx.cmd->SetPipelineState(pso);
                             ctx.bindFrameConstants(constants);
                             ctx.computeConstants(k, 4);
                             ctx.cmd->Dispatch((gridX + 7) / 8, (gridY + 7) / 8, 1);
                         });
        return hiz;
    }
};

// A FogProbe.hlsl pass in a frame: 'count' queries, outPer float4 each. words: P[2], P[3]. lut: MODE 4's transmittance
// LUT (its SRV goes to P[2].x); volume: the fog's volume the frame constants' record names (MODE 3; invalid: none).
std::shared_ptr<std::vector<uint8_t>> probePass(TestFrame& tf, FramePassContext& fc, int mode, const std::vector<float4>& in, uint32_t count, uint32_t outPer,
                                                D3D12_GPU_VIRTUAL_ADDRESS constants, const std::array<uint32_t, 8>& words, TextureRef lut, TextureRef volume)
{
    const BufferRef queries = tf.uploadBuffer(fc, in.data(), in.size() * 16, 16, "fog probe queries");
    const BufferRef output = fc.graph.createBuffer(BufferDesc{ "fog probe output", (uint64_t)count * outPer * 16, 16 });
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/FogProbe.MODE" + std::to_string(mode));
    fc.graph.addPass("s.test.fogprobe", QueueType::Compute,
                     [&](PassBuilder& b) {
                         b.use(queries, Use::SrvCompute);
                         b.use(output, Use::UavCompute);
                         if (lut.valid()) b.use(lut, Use::SrvCompute);
                         if (volume.valid()) b.use(volume, Use::SrvCompute);
                     },
                     [=](PassContext& ctx) {
                         uint32_t k[16] = { ctx.srv(queries), ctx.uav(output), count, 0, 0, 0, 0, 0 };
                         for (int i = 0; i < 8; ++i) k[8 + i] = words[i];
                         if (lut.valid()) k[8] = ctx.srv(lut);
                         ctx.cmd->SetPipelineState(pso);
                         ctx.bindFrameConstants(constants);
                         ctx.computeConstants(k, 16);
                         ctx.cmd->Dispatch((count + 63) / 64, 1, 1);
                     });
    return tf.readbackBuffer(fc, output, (uint64_t)count * outPer * 16);
}

// What a run of frames is asked for and what it gives back.
struct Options
{
    int frames = 2;
    bool history = false;                               // atmosphere.fog.history_weight as the file has it (false: 0)
    std::vector<std::pair<uint32_t, uint32_t>> columns;  // the cells' columns whose references need the sun
    std::vector<float4> reader;                          // FogProbe MODE 3 queries of the read frame (3 float4 each)
    bool debugView = false;                              // atmosphere.fog.debug_view: the image read back
    bool mirror = false;                                 // a planar reflection view across mirrorPlane too
    float4 mirrorPlane{};
    int phase = -1;                                      // the read frame's index mod 16 (-1: any)
    int startPhase = -1;                                 // the first frame's index mod 16 (-1: any): runs of the same frames
    float3 rebase{};                                     // GpuScene::rebase before the first frame
    float ev100 = 14;                                    // the view's exposure (EV100) ...
    int evFrames = 0;                                    // ... and that of the run's last evFrames frames
    float evThen = 14;
};
struct Result
{
    uint32_t word = 0;       // tracks::fogParams of the read frame
    bool hasVolume = false;  // FrameResources::fogVolume was there
    FogView fog;             // FrameResources::fog
    Model model;             // the read frame's main view
    Volume volume;
    std::vector<float4> sun, reader;  // MODE 4 (per column grid.z + 1 values), MODE 3 (6 float4 per query)
    std::vector<uint8_t> debug;
    bool hasPlanar = false;
    Model planarModel;
    Volume planar;
    std::vector<float4> planarSun;
    uint32_t errors = 0;
    double exposure = 1;     // the read frame's
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, cpuOnly = false;
        uint32_t W = 1920, H = 1080;
        std::vector<std::string> overrides;
        std::set<std::string> only;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else if (a == "--cpu") cpuOnly = true;
            else if (a == "--width") W = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--height") H = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--set") overrides.push_back(argv[++i]);
            else if (a == "--only")
            {
                std::string list = argv[++i];
                size_t at = 0;
                while (at <= list.size())
                {
                    const size_t comma = std::min(list.find(',', at), list.size());
                    if (comma > at) only.insert(list.substr(at, comma - at));
                    at = comma + 1;
                }
            }
        }
        auto want = [&](const char* name) { return only.empty() || only.count(name) != 0; };
        int failures = 0;
        auto report = [&](bool ok, const std::string& what, double value, double limit) {
            logf("%-88s %.4g (limit %.4g) %s\n", what.c_str(), value, limit, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        std::mt19937 rng(11);
        auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };

        // ---- 1. The closed form (CPU): fogOpticalDepth against the integral of fogExtinctionAt.
        struct ClosedSet
        {
            Medium medium;
            std::vector<float4> queries;  // 3 per case (FogProbe MODE 0)
        };
        std::vector<ClosedSet> closedSets;
        if (want("closed"))
        {
            const double media[10][4] = {  // density, falloff, height, start
                { 0.002, 0.02, 0, 0 },   { 0.002, 0.0, 0, 0 },     { 0.05, 0.02, 35, 0 },  { 0.002, 0.005, -200, 12.5 }, { 0.01, 0.1, 20, 0 },
                { 0.0005, 0.5, 0, 0 },   { 0.002, 0.02, 0, 300 },  { 0.05, 0.1, 35, 12.5 }, { 2e-5, 0.02, 0, 0 },         { 0.2, 0.02, 0, 0 } };
            const double slopes[15] = { 1, -1, 0, 1e-6, -1e-6, 1e-4, -1e-4, 1e-3, -1e-3, 0.02, -0.02, 0.3, -0.3, 0.8, -0.8 };
            const double segments[9][2] = { { 0, 0.0928 }, { 3.1, 3.4 }, { 77, 80 }, { 80, 121.7 }, { 992, 1509 }, { 0, 65536 }, { 0, 500 }, { 11, 14.5 }, { 250, 420 } };
            double worst = 0, worstUp = 0, worstDown = 0, worstLevel = 0, worstHold = 0, largest = 0;
            uint32_t cases = 0, capped = 0, crossHeight = 0, crossHold = 0, started = 0;
            for (const auto& md : media)
            {
                ClosedSet set;
                set.medium.density = md[0];
                set.medium.falloff = md[1];
                set.medium.height = md[2];
                set.medium.start = md[3];
                const double hold = md[1] > 0 ? md[2] - 6.0 / md[1] : -1e30;  // the height under which the density is held
                std::vector<double> heights = { md[2] + 0.3, md[2] - 0.3, md[2] + 40, md[2] - 40, md[2] + 400, md[2] - 400 };
                if (md[1] > 0)
                    for (double dh : { 0.5, -0.5, 25.0, -25.0 }) heights.push_back(hold + dh);
                for (double y : heights)
                    for (double slope : slopes)
                        for (const auto& sg : segments)
                        {
                            const double azimuth = uni(0, 2 * kPi), level = std::sqrt(std::max(1 - slope * slope, 0.0));
                            // the kernel's inputs are floats: the references take the same values
                            const float of[3] = { (float)uni(-50, 50), (float)y, (float)uni(-50, 50) };
                            float df[3] = { (float)(level * std::cos(azimuth)), (float)slope, (float)(level * std::sin(azimuth)) };
                            const float t0 = (float)sg[0], t1 = (float)sg[1];
                            const D3 o{ of[0], of[1], of[2] }, d{ df[0], df[1], df[2] };
                            const double exact = opticalDepthNumeric(set.medium, o, d, t0, t1), closed = opticalDepthClosed(set.medium, o, d, t0, t1);
                            const double err = std::abs(closed - exact) / std::max(exact, 1e-12);
                            const double ya = o.y + d.y * std::max<double>(t0, md[3]), yb = o.y + d.y * t1;
                            const bool overHold = md[1] > 0 && std::min(ya, yb) < hold && std::max(ya, yb) > hold;
                            ++cases;
                            capped += exact >= 64.0 ? 1 : 0;
                            crossHeight += std::min(ya, yb) < md[2] && std::max(ya, yb) > md[2] ? 1 : 0;
                            crossHold += overHold ? 1 : 0;
                            started += md[3] > t0 && md[3] < t1 ? 1 : 0;
                            largest = std::max(largest, exact);
                            if (err > 1e-7 && worst <= 1e-7)
                                logf("  closed form: density %g falloff %g height %g start %g | origin y %g dir y %g t %g..%g: closed %.9g integral %.9g\n", md[0], md[1], md[2],
                                     md[3], o.y, d.y, (double)t0, (double)t1, closed, exact);
                            worst = std::max(worst, err);
                            if (overHold) worstHold = std::max(worstHold, err);
                            else if (slope > 1e-3) worstUp = std::max(worstUp, err);
                            else if (slope < -1e-3) worstDown = std::max(worstDown, err);
                            else worstLevel = std::max(worstLevel, err);
                            set.queries.push_back({ of[0], of[1], of[2], t0 });
                            set.queries.push_back({ df[0], df[1], df[2], t1 });
                            set.queries.push_back({ (float)(y + uni(-30, 30)), 0, 0, 0 });
                        }
                closedSets.push_back(set);
            }
            logf("closed form: %u rays over %zu media; %u cross the fog's height, %u cross the held height, %u start inside the segment, %u at the cap of 64; "
                 "largest optical depth %.4g\n", cases, closedSets.size(), crossHeight, crossHold, started, capped, largest);
            report(crossHeight > 100 && crossHold > 100 && started > 100 && capped > 10, "closed form: the cases cover the height, the hold, the start and the cap (fewest)",
                   std::min({ crossHeight, crossHold, started }), 100);
            report(worstUp <= 1e-7, "closed form vs the integral: rays going up (rel.)", worstUp, 1e-7);
            report(worstDown <= 1e-7, "closed form vs the integral: rays going down (rel.)", worstDown, 1e-7);
            report(worstLevel <= 1e-7, "closed form vs the integral: level rays, |slope| <= 1e-3 (rel.)", worstLevel, 1e-7);
            report(worstHold <= 1e-7, "closed form vs the integral: segments across the held height (rel.)", worstHold, 1e-7);
            report(worst <= 1e-7, "closed form vs the integral: all rays (rel.)", worst, 1e-7);
        }

        // ---- 2. The grid mapping (CPU).
        FogView defaultFog;
        {
            QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
            for (const std::string& o : overrides) q.applyOverride(o);
            FrameContext frame;
            frame.fog.enabled = true;  // (the grid is the file's whatever the medium)
            defaultFog = shadow::fogViewFor(q, frame, W, H);
        }
        const GridCpu defaultGrid = gridOf(defaultFog);
        std::vector<float4> gridQueries;
        if (want("grid"))
        {
            const GridCpu& g = defaultGrid;
            logf("grid: %u x %u x %u cells of %u px to %.6g m (k %.6g, b %.6g), %u far slices to %.6g m\n", g.x, g.y, g.z, g.cellPx, g.farM, g.k, g.b, g.zFar, g.farEndM);
            report(g.x == (W + g.cellPx - 1) / g.cellPx && g.y == (H + g.cellPx - 1) / g.cellPx, "grid: the cells cover the view (cells across)", g.x, (W + g.cellPx - 1) / g.cellPx);
            report(sliceOfDepth(g, 0) == 0 && std::abs(sliceOfDepth(g, g.farM) - g.z) <= 1e-4, "grid: slice 0 at the camera, slice z at the cells' end (slices off)",
                   std::abs(sliceOfDepth(g, g.farM) - g.z), 1e-4);
            double worstTrip = 0, worstFar = 0;
            bool rising = true;
            for (int i = 0; i < 4000; ++i)
            {
                const double depth = g.farM * std::pow(uni(0, 1), 3.0);
                worstTrip = std::max(worstTrip, std::abs(depthOfSlice(g, sliceOfDepth(g, depth)) - depth) / (depth + 1 / g.k));
            }
            for (uint32_t s = 0; s < g.z; ++s) rising = rising && depthOfSlice(g, s + 1.0) > depthOfSlice(g, s);
            report(worstTrip <= 1e-12 && rising, "grid: depth -> slice -> depth, slices in rising depth (rel. to depth + 1 / k)", worstTrip, 1e-12);
            const double first = depthOfSlice(g, 1), lastSlice = depthOfSlice(g, g.z) - depthOfSlice(g, g.z - 1.0);
            logf("grid: the first slice is %.4f m, the last %.3f m\n", first, lastSlice);
            if (g.z == 96 && g.farM == 80)
                report(first > 0.085 && first < 0.10 && lastSlice > 2.8 && lastSlice < 3.1, "grid: 9 cm slices at the camera, 3 m at 80 m (FogVolume.hlsli; first slice, m)", first, 0.0928);
            if (g.zFar != 0)
            {
                const double ratio = std::pow(g.farEndM / g.farM, 1.0 / g.zFar);
                for (uint32_t i = 0; i < g.zFar; ++i) worstFar = std::max(worstFar, std::abs(farDepth(g, i + 1.0) / farDepth(g, i) - ratio) / ratio);
                worstFar = std::max({ worstFar, std::abs(farDepth(g, 0) - g.farM) / g.farM, std::abs(farDepth(g, g.zFar) - g.farEndM) / g.farEndM });
                report(worstFar <= 1e-12, "grid: far slices from the cells' end to the far end in equal steps of log depth (rel.)", worstFar, 1e-12);
                const double eps = 1e-6;
                const double step = std::abs(readerCoord(g, g.farM * (1 + eps)) - readerCoord(g, g.farM * (1 - eps)));
                // (the coordinate's slope either side of the end: z b k / ((farM k + 1) ln 2) and zFar / (ln 2 log2(far end / farM)) per unit of relative depth)
                report(step <= 2 * eps * g.farM * (g.b * g.k / std::log(2.0)) + 1e-4, "grid: the reader's slice coordinate is continuous at the cells' end (slices)", step, 1e-4);
            }
            double worstSegment = 0;
            for (int i = 0; i < 2000; ++i)
            {
                double za = uni(0, 80), zb = za + uni(0.01, 4);
                const double limit = uni(0, 90), length = zb - za, a0 = za;
                segment(za, zb, limit);
                // in front of the limit, not behind the camera, its length kept unless the camera cuts it
                const double want0 = std::max(a0 - std::max(a0 + length - limit, 0.0), 0.0);
                worstSegment = std::max({ worstSegment, zb - std::max(limit, zb), -za, std::abs(za - want0), a0 + length > limit ? std::abs(zb - limit) : std::abs(zb - a0 - length) });
            }
            report(worstSegment <= 1e-12, "grid: a cell's segment stays in front of its surface with its length (m)", worstSegment, 1e-12);
            // the GPU's queries: depths and slice coordinates over both ranges
            for (double x : { 0.0, 0.01, 0.0928, 1.0, 7.7, 16.0, 40.0, 79.9, 80.0, 96.0 }) gridQueries.push_back({ (float)x, 0, 1, 2 });
            for (int i = 0; i < 502; ++i)
            {
                const double za = uni(0, 80);
                gridQueries.push_back({ (float)(i < 251 ? uni(0, 96) : 96 * std::pow(uni(0, 1), 4.0)), (float)za, (float)(za + uni(0.01, 4)), (float)uni(0, 90) });
            }
        }

        // ---- 8. The density's variation (CPU).
        std::vector<float4> noiseQueries;
        if (want("noise"))
        {
            double sum = 0, sum2 = 0;
            float lo = 1e30f, hi = -1e30f;
            for (int z = 0; z < 256; ++z)
                for (int y = 0; y < 256; ++y)
                    for (int x = 0; x < 256; ++x)
                    {
                        const float v = latticeValue(x, y, z);
                        sum += v;
                        sum2 += (double)v * v;
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
            const double n3 = 256.0 * 256.0 * 256.0, latticeMean = sum / n3, latticeVar = sum2 / n3 - latticeMean * latticeMean;
            const double se = std::sqrt(1.0 / 3.0 / n3);  // values uniform in [-1, 1]: variance 1/3
            logf("noise: lattice of 2^24 values in [%.6f, %.6f], mean %.3e (standard error %.3e), variance %.5f\n", lo, hi, latticeMean, se, latticeVar);
            report(lo >= -1.0f && hi <= 1.0f, "noise: lattice values inside [-1, 1] (largest magnitude)", std::max(-lo, hi), 1);
            report(std::abs(latticeMean) <= 3 * se, "noise: the lattice's mean over its period is zero (3 standard errors)", std::abs(latticeMean), 3 * se);
            report(std::abs(latticeVar - 1.0 / 3.0) <= 0.01, "noise: the lattice's variance is that of uniform values (1/3)", latticeVar, 1.0 / 3.0);
            // The noise at random points of the period: its mean is the lattice's (the interpolation's weights of a lattice
            // value integrate to the same share for every value), so the density's mean is 1 + 2 amount x that.
            const uint32_t N = 1u << 22;
            std::mt19937 nr(5);
            std::uniform_real_distribution<float> place(0.0f, 256.0f);
            double noiseSum = 0, noise2 = 0, scaleSum[3] = { 0, 0, 0 };
            float noiseLo = 1e30f, noiseHi = -1e30f, scaleLo[3] = { 1e30f, 1e30f, 1e30f }, scaleHi[3] = { -1e30f, -1e30f, -1e30f };
            const float amounts[3] = { 0.3f, 0.5f, 1.0f };
            for (uint32_t i = 0; i < N; ++i)
            {
                const float px = place(nr), py = place(nr), pz = place(nr);
                const float n = densityNoise(px, py, pz);
                noiseSum += n;
                noise2 += (double)n * n;
                noiseLo = std::min(noiseLo, n);
                noiseHi = std::max(noiseHi, n);
                for (int a = 0; a < 3; ++a)
                {
                    const float s = std::max(1.0f + amounts[a] * 2.0f * n, 0.0f);
                    scaleSum[a] += s;
                    scaleLo[a] = std::min(scaleLo[a], s);
                    scaleHi[a] = std::max(scaleHi[a], s);
                }
            }
            const double noiseMean = noiseSum / N, noiseSd = std::sqrt(noise2 / N - noiseMean * noiseMean), sample = 3 * noiseSd / std::sqrt((double)N);
            logf("noise: two octaves at %u random points: mean %.3e, standard deviation %.4f, range [%.4f, %.4f]\n", N, noiseMean, noiseSd, noiseLo, noiseHi);
            report(noiseLo >= -1.0f && noiseHi <= 1.0f, "noise: the variation stays inside [-1, 1] (largest magnitude)", std::max(-noiseLo, noiseHi), 1);
            report(std::abs(noiseMean - latticeMean) <= sample, "noise: the variation's mean is the lattice's (3 standard errors of the sample)", std::abs(noiseMean - latticeMean), sample);
            for (int a = 0; a < 2; ++a)
            {
                const double mean = scaleSum[a] / N, limit = 2 * amounts[a] * (3 * se + sample);
                report(std::abs(mean - 1) <= limit, format("noise amount %.1f: the density's mean is the closed form's (mean factor - 1)", amounts[a]), std::abs(mean - 1), limit);
                report(scaleLo[a] >= 1 - 2 * amounts[a] - 1e-6f && scaleHi[a] <= 1 + 2 * amounts[a] + 1e-6f && scaleLo[a] >= 0,
                       format("noise amount %.1f: the factor stays within 1 +- 2 amount and above 0 (lowest)", amounts[a]), scaleLo[a], 1 - 2 * amounts[a]);
            }
            logf("noise amount 1.0 (the range's end; the factor is cut at 0 below noise -0.5): mean factor %.5f, range [%.4f, %.4f]\n", scaleSum[2] / N, scaleLo[2], scaleHi[2]);
            report(scaleLo[2] >= 0, "noise amount 1.0: the factor is not negative (lowest)", scaleLo[2], 0);
            // The lattice's period: 256 along each axis and not 128 (coordinates on a 1/64 grid, so + 256 is exact in float).
            uint32_t differ256 = 0, same128 = 0;
            for (int i = 0; i < 4096; ++i)
            {
                const float px = (float)(int)uni(-8192, 8192) / 64.0f, py = (float)(int)uni(-8192, 8192) / 64.0f, pz = (float)(int)uni(-8192, 8192) / 64.0f;
                const float base = densityScale(px, py, pz, 0.3f);
                const int axisIndex = i % 3;
                const float sx = axisIndex == 0 ? 1.0f : 0.0f, sy = axisIndex == 1 ? 1.0f : 0.0f, sz = axisIndex == 2 ? 1.0f : 0.0f;
                differ256 += densityScale(px + 256 * sx, py + 256 * sy, pz + 256 * sz, 0.3f) != base ? 1 : 0;
                same128 += densityScale(px + 128 * sx, py + 128 * sy, pz + 128 * sz, 0.3f) == base ? 1 : 0;
                if (i < 1024)
                {
                    noiseQueries.push_back({ px, py, pz, 0.3f });
                    noiseQueries.push_back({ px + 256 * sx, py + 256 * sy, pz + 256 * sz, 0.3f });
                }
            }
            report(differ256 == 0, "noise: the variation repeats every 256 lattice points along each axis (points differing)", differ256, 0);
            report(same128 < 64, "noise: and not every 128 (points equal by chance, of 4096)", same128, 64);
            for (int i = 0; i < 2048; ++i)
                noiseQueries.push_back({ (float)uni(-300, 600), (float)uni(-300, 600), (float)uni(-300, 600), (float)(i % 4 == 0 ? 1.0 : (i % 4 == 1 ? 0.5 : 0.3)) });
        }
        if (cpuOnly)
        {
            logf(failures ? "FAIL (%d)\n" : "PASS (the parts without a device)\n", failures);
            return failures ? 1 : 0;
        }

        TestFrame tf(debugLayer);
        for (const std::string& o : overrides) tf.quality.applyOverride(o);
        tf.quality.applyOverride("atmosphere.fog.enabled=false");         // the frame's medium (FrameContext::fog) decides
        tf.quality.applyOverride("atmosphere.fog.indirect_light=false");  // (no translucency volume in this frame anyway)
        TestRaster raster(tf);
        raster.install();
        TestHiz hiz;
        FramePassContext* current = nullptr;
        tf.fogSecondary = [&](const ViewDesc& v, D3D12_GPU_VIRTUAL_ADDRESS address) { return current ? tracks::fogParamsSecondary(*current, v, address) : 0u; };
        const std::array<uint32_t, 8> noWords{};

        // Nothing in the view: a box behind the camera (its shadow falls far from the view's fog).
        scene::Scene open;
        open.name = "fog open";
        open.materials.push_back({});
        open.meshes.push_back(boxMesh("far box", { 1, 1, 1 }));
        open.instances.push_back(instanceAt(0, { -3000, 1, -3000 }));
        {
            scene::Camera c;
            c.name = "main";
            c.position = { 3, 1.7f, -2 };
            c.forward = normalize(float3{ 0.1f, -0.02f, 1 });
            open.cameras.push_back(c);
        }
        tf.setScene(open);
        tf.frame.mainView = ViewDesc::fromCamera(open.cameras[0], W, H, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;

        // A probe on its own (the functions that need no frame).
        auto probe = [&](int mode, const std::vector<float4>& in, uint32_t count, uint32_t outPer, const std::array<uint32_t, 8>& words) {
            std::shared_ptr<std::vector<uint8_t>> rb;
            tf.fogParams = 0;
            tf.run([&](FramePassContext& fc) { rb = probePass(tf, fc, mode, in, count, outPer, fc.frameConstantsFor(fc.frame.mainView), words, TextureRef{}, TextureRef{}); });
            return floatsOf(*rb);
        };

        // The model of a frame from the test's inputs: FrameContext::fog and fogVolumes, the quality keys, the scene.
        auto makeModel = [&](const scene::Scene& sc, const ViewDesc& view, uint64_t first, uint64_t readFrame, double time, bool history, bool planar, double exposure) {
            Model m;
            m.view = { view, d3(view.position) };
            m.exposure = exposure;
            m.cuts = history && !planar ? 10 : 1;
            const FogView fv = shadow::fogViewFor(tf.quality, tf.frame, view.width, view.height);
            m.grid = gridOf(fv);
            m.origin = d3(tf.gpuScene.originOffset());
            const FogDesc& fd = tf.frame.fog;
            if (fd.enabled)
            {
                m.medium.density = fd.density;
                m.medium.falloff = fd.heightFalloff;
                m.medium.height = (double)fd.height - m.origin.y;
                m.medium.g = fd.phaseG;
                m.medium.start = fd.startDistance;
                for (int k = 0; k < 3; ++k) m.medium.albedo[k] = fd.albedo[k];
            }
            else
            {
                m.medium.g = tf.quality.number("atmosphere.fog.phase_g");  // (a volume alone: the file's phase function)
            }
            // the density's variation: the frame's with its medium, the file's otherwise; it drifts with the scene's wind x
            // noise_wind_scale, in still air along x at noise_drift_mps
            const double amount = fd.enabled ? fd.noiseAmount : tf.quality.number("atmosphere.fog.noise_amount");
            const double scale = fd.enabled ? fd.noiseScale : tf.quality.number("atmosphere.fog.noise_scale_m");
            if (amount > 0)
            {
                m.noiseAmount = amount;
                m.noiseInvScale = (double)(1.0f / (float)scale);
                const double drift = tf.quality.number("atmosphere.fog.noise_drift_mps"), speed = (double)sc.windSpeed * tf.quality.number("atmosphere.fog.noise_wind_scale");
                const D3 wind = d3(sc.windDirection);
                D3 velocity{ drift, 0, 0 };
                if (speed > drift && len(wind) > 1e-6) velocity = wind * (speed / len(wind));
                const double o[3] = { m.origin.x, m.origin.y, m.origin.z }, v[3] = { velocity.x, velocity.y, velocity.z }, stretch[3] = { 1, 2, 1 };
                for (int k = 0; k < 3; ++k)
                {
                    const double lattice = std::fmod((o[k] - v[k] * time) * stretch[k] / scale, 256.0);
                    m.noiseOffset[k] = (float)(lattice < 0 ? lattice + 256.0 : lattice);
                }
            }
            if (tf.quality.boolean("atmosphere.fog.local_volumes")) m.volumes = tf.frame.fogVolumes;
            m.sun = ref::normalize(d3(sc.sun.direction));
            const D3 colour = d3(sc.sun.color);
            m.E[0] = sc.sun.illuminance * colour.x, m.E[1] = sc.sun.illuminance * colour.y, m.E[2] = sc.sun.illuminance * colour.z;
            if (planar)
            {
                // (its own volume: no history, the cells sampled at their centres)
                m.clip[0] = view.clipPlane.x, m.clip[1] = view.clipPlane.y, m.clip[2] = view.clipPlane.z, m.clip[3] = view.clipPlane.w;
                m.samples.push_back({ 0.5, 0.5, 0.5, 1 });
                return m;
            }
            // the frames' places in the cells; with the history each frame keeps (1 - weight) of its own sample
            const double weight = history ? tf.quality.number("atmosphere.fog.history_weight") : 0.0;
            double share[16] = {};
            for (uint64_t f = weight > 0 ? first : readFrame; f <= readFrame; ++f)
                share[f % 16] += (f == first && weight > 0 ? 1.0 : 1.0 - weight) * std::pow(weight, (double)(readFrame - f));
            if (!(weight > 0)) share[readFrame % 16] = 1;
            for (uint32_t i = 0; i < 16; ++i)
                if (share[i] > 0) m.samples.push_back({ halton(i + 1, 2), halton(i + 1, 3), halton(i + 1, 5), share[i] });
            return m;
        };

        // Frames of a scene with the fog as tf.frame has it; the last one is read back.
        auto run = [&](const scene::Scene& sc, const Options& o) {
            Result r;
            tf.quality.applyOverride(o.history ? format("atmosphere.fog.history_weight=%.9g", (double)defaultFog.historyWeight) : std::string("atmosphere.fog.history_weight=0"));
            tf.quality.applyOverride(o.debugView ? "atmosphere.fog.debug_view=true" : "atmosphere.fog.debug_view=false");
            tf.setScene(sc);
            if (o.rebase.x != 0 || o.rebase.y != 0 || o.rebase.z != 0)
            {
                tf.gpuScene.rebase(o.rebase);
                tf.gpuScene.flushUpdates(tf.frame.frameIndex, 2, tf.shaders);
            }
            tf.frame.mainView = ViewDesc::fromCamera(sc.cameras[0], W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
            tf.frame.deltaTime = 1.0f / 60;
            // (frames of nothing until the run starts at its place in the cells' 16 frames)
            while (o.startPhase >= 0 && tf.frame.frameIndex % 16 != (uint64_t)o.startPhase) probe(2, std::vector<float4>(1), 1, 1, noWords);
            int frames = o.frames;
            if (o.phase >= 0)
                while ((tf.frame.frameIndex + frames - 1) % 16 != (uint64_t)o.phase) ++frames;
            const uint64_t first = tf.frame.frameIndex;
            const uint32_t cellPx = (uint32_t)tf.quality.integer("atmosphere.fog.cell_px");
            ViewDesc mirrorView = o.mirror ? ViewDesc::planarReflection(tf.frame.mainView, o.mirrorPlane, 0, 0, W, H) : ViewDesc{};
            auto exposureOf = [](float ev100) { return (double)(1.0f / (1.2f * std::exp2(ev100))); };
            r.exposure = exposureOf(o.evFrames > 0 ? o.evThen : o.ev100);
            const double leastExposure = o.evFrames > 0 ? std::min(exposureOf(o.ev100), exposureOf(o.evThen)) : exposureOf(o.ev100);
            for (int f = 0; f < frames; ++f)
            {
                const bool read = f + 1 == frames;
                tf.frame.discontinuity = f == 0 ? kDiscontinuityRestore : 0u;  // (every run starts without history)
                tf.frame.originShift = f == 0 ? o.rebase : float3{};
                tf.frame.mainView.ev100 = f >= frames - o.evFrames ? o.evThen : o.ev100;
                mirrorView.ev100 = tf.frame.mainView.ev100;
                std::shared_ptr<std::vector<uint8_t>> volumeRb, sunRb, readerRb, debugRb, planarRb;
                std::vector<float4> sunIn;
                size_t mainPoints = 0;
                if (read)
                {
                    r.model = makeModel(sc, tf.frame.mainView, first, tf.frame.frameIndex, tf.frame.time, o.history, false, leastExposure);
                    for (const auto& c : o.columns) sunPoints(r.model, c.first, c.second, sunIn);
                    mainPoints = sunIn.size();
                    if (o.mirror)
                    {
                        r.planarModel = makeModel(sc, mirrorView, first, tf.frame.frameIndex, tf.frame.time, false, true, r.exposure);
                        for (const auto& c : o.columns) sunPoints(r.planarModel, c.first, c.second, sunIn);
                    }
                }
                tf.run([&](FramePassContext& fc) {
                    current = &fc;
                    tf.fogParams = tracks::fogParams(fc, fc.frame.mainView);  // S: before the main view's constants (FrameRenderer)
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    tracks::atmosphere(fc);
                    raster.mainView(fc, main);
                    main.hiz = hiz.record(fc, main.depth, main.frameConstants, W, H, cellPx);
                    tracks::shadowPages(fc, main);
                    tracks::froxels(fc, main);
                    ViewResources planar;
                    if (o.mirror)
                    {
                        planar.view = mirrorView;
                        planar.frameConstants = fc.frameConstantsFor(planar.view);
                        raster.mainView(fc, planar);
                        shadow::recordPlanarFroxels(fc, planar);
                    }
                    if (read)
                    {
                        r.word = tf.fogParams;
                        r.fog = fc.resources.fog;
                        r.hasVolume = fc.resources.fogVolume.valid();
                        if (r.hasVolume) volumeRb = tf.readback(fc, fc.resources.fogVolume);
                        if (!sunIn.empty())
                            sunRb = probePass(tf, fc, 4, sunIn, (uint32_t)sunIn.size(), 1, main.frameConstants, noWords, fc.resources.transmittanceLut, TextureRef{});
                        if (!o.reader.empty())
                            readerRb = probePass(tf, fc, 3, o.reader, (uint32_t)(o.reader.size() / 3), 6, main.frameConstants, noWords, TextureRef{}, fc.resources.fogVolume);
                        if (o.debugView && fc.resources.fogDebug.valid()) debugRb = tf.readback(fc, fc.resources.fogDebug);
                        r.hasPlanar = planar.fogVolume.valid();
                        if (r.hasPlanar) planarRb = tf.readback(fc, planar.fogVolume);
                    }
                    current = nullptr;
                });
                tf.frame.time += tf.frame.deltaTime;
                if (read)
                {
                    if (volumeRb) r.volume = { *volumeRb, r.fog.gridX, r.fog.gridY, r.fog.gridZ + r.fog.farSlices, r.exposure };
                    if (planarRb) r.planar = { *planarRb, r.planarModel.grid.x, r.planarModel.grid.y, r.planarModel.grid.z + r.planarModel.grid.zFar, r.exposure };
                    if (sunRb)
                    {
                        const std::vector<float4> all = floatsOf(*sunRb);
                        r.sun.assign(all.begin(), all.begin() + mainPoints);
                        r.planarSun.assign(all.begin() + mainPoints, all.end());
                    }
                    if (readerRb) r.reader = floatsOf(*readerRb);
                    if (debugRb) r.debug = *debugRb;
                }
            }
            r.errors = shadow::stats(tf.trackState).errorBitsSeen;
            return r;
        };
        auto setFog = [&](double density, double falloff, double height, double g, std::array<float, 3> albedo, double start = 0, double noise = 0, double noiseScale = 20) {
            FogDesc& fd = tf.frame.fog;
            fd = FogDesc{};
            fd.enabled = true;
            fd.density = (float)density;
            fd.heightFalloff = (float)falloff;
            fd.height = (float)height;
            fd.phaseG = (float)g;
            fd.startDistance = (float)start;
            fd.noiseAmount = (float)noise;
            fd.noiseScale = (float)noiseScale;
            for (int k = 0; k < 3; ++k) fd.albedo[k] = albedo[k];
        };
        const GridCpu grid = defaultGrid;
        const uint32_t gz = grid.z, total = grid.z + grid.zFar, perColumn = grid.z + 1;
        // Columns over the view: the corners, the edges and the inside.
        std::vector<std::pair<uint32_t, uint32_t>> spread;
        for (uint32_t cy : { 0u, grid.y / 6, grid.y / 3, grid.y / 2, 2 * grid.y / 3, 5 * grid.y / 6, grid.y - 1 })
            for (uint32_t cx : { 0u, grid.x / 5, 2 * grid.x / 5, grid.x / 2, 3 * grid.x / 5, 4 * grid.x / 5, grid.x - 1 }) spread.push_back({ cx, cy });
        // A frame's volume against the twin over its columns: near and far slices.
        auto againstTwin = [&](const Result& r, const std::vector<std::pair<uint32_t, uint32_t>>& columns, const std::string& label, const std::vector<double>* extra = nullptr) {
            Tally nearT, farT;
            for (size_t i = 0; i < columns.size(); ++i)
            {
                const Column c = twinColumn(r.model, columns[i].first, columns[i].second, &r.sun[i * perColumn]);
                compareFaces(r.volume, columns[i].first, columns[i].second, c, 0, gz, extra, nearT, label.c_str());
                compareFaces(r.volume, columns[i].first, columns[i].second, c, gz, total, extra, farT, label.c_str());
            }
            logf("%s: %u near faces, worst rel. L %.3e T %.3e; %u far faces, worst rel. L %.3e T %.3e\n", label.c_str(), nearT.faces, nearT.worstL, nearT.worstT, farT.faces,
                 farT.worstL, farT.worstT);
            report(nearT.faces > 0 && nearT.over == 0, label + ": cells' faces outside the tolerance", nearT.over, 0);
            report(farT.faces > 0 && farT.over == 0, label + ": far slices' faces outside the tolerance", farT.over, 0);
            report(nearT.notFinite + farT.notFinite == 0, label + ": texels that are not finite", nearT.notFinite + farT.notFinite, 0);
        };
        auto volumeThere = [&](const Result& r, const std::string& label) {
            const bool there = r.word != 0 && r.hasVolume && r.fog.on && r.volume.bytes.size() >= (size_t)TestFrame::rowPitch(r.fog.gridX, 8) * r.fog.gridY * (total - 1);
            report(there && r.fog.gridX == grid.x && r.fog.gridY == grid.y && r.fog.gridZ == grid.z && r.fog.farSlices == grid.zFar, label + ": the frame has the fog's record and volume", there, 1);
            return there;
        };

        // ---- 1, 2, 8 on the GPU: the kernels' functions against the twins at the same queries.
        if (want("closed"))
        {
            double worstTau = 0, worstSigma = 0;
            uint32_t n = 0;
            for (const ClosedSet& set : closedSets)
            {
                const uint32_t count = (uint32_t)(set.queries.size() / 3);
                const std::array<uint32_t, 8> words = { bitsOf((float)set.medium.density), bitsOf((float)set.medium.falloff), bitsOf((float)set.medium.height), bitsOf(0.2f),
                                                        bitsOf(1.0f), bitsOf(1.0f), bitsOf(1.0f), bitsOf((float)set.medium.start) };
                Medium asFloats = set.medium;  // the kernel's medium is floats
                asFloats.density = (float)set.medium.density, asFloats.falloff = (float)set.medium.falloff, asFloats.height = (float)set.medium.height, asFloats.start = (float)set.medium.start;
                const std::vector<float4> out = probe(0, set.queries, count, 1, words);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float4 q0 = set.queries[3 * i], q1 = set.queries[3 * i + 1], q2 = set.queries[3 * i + 2];
                    const double want0 = opticalDepthClosed(asFloats, D3{ q0.x, q0.y, q0.z }, D3{ q1.x, q1.y, q1.z }, q0.w, q1.w), wantS = extinctionAt(asFloats, q2.x);
                    const double eTau = std::abs(out[i].x - want0) / std::max(want0, 2e-4), eSigma = std::abs(out[i].y - wantS) / std::max(wantS, 1e-30);
                    if (eTau > 5e-4 && worstTau <= 5e-4)
                        logf("  closed form on the GPU: density %g falloff %g height %g | origin y %g dir y %g t %g..%g: gpu %.9g twin %.9g\n", asFloats.density, asFloats.falloff,
                             asFloats.height, (double)q0.y, (double)q1.y, (double)q0.w, (double)q1.w, (double)out[i].x, want0);
                    worstTau = std::max(worstTau, eTau);
                    worstSigma = std::max(worstSigma, eSigma);
                    ++n;
                }
            }
            report(n > 1000 && worstTau <= 5e-4, "closed form on the GPU vs the twin (rel. to the optical depth, floor 2e-4)", worstTau, 5e-4);
            report(worstSigma <= 1e-5, "fogExtinctionAt on the GPU vs the twin (rel.)", worstSigma, 1e-5);
        }
        if (want("grid"))
        {
            const GridCpu& g = defaultGrid;
            const std::array<uint32_t, 8> words = { g.x | g.y << 16, g.z | g.cellPx << 16 | g.zFar << 24, bitsOf(defaultFog.farM), bitsOf(defaultFog.k),
                                                    bitsOf(defaultFog.b), bitsOf(defaultFog.farEndM), 0, 0 };
            const uint32_t count = (uint32_t)gridQueries.size();
            const std::vector<float4> out = probe(1, gridQueries, count, 2, words);
            double worstSlice = 0, worstDepth = 0, worstFar = 0, worstTrip = 0, worstSegment = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float4 q = gridQueries[i], a = out[2 * i], b = out[2 * i + 1];
                worstSlice = std::max(worstSlice, std::abs(a.x - sliceOfDepth(g, q.x)));
                worstDepth = std::max(worstDepth, std::abs(a.y - depthOfSlice(g, q.x)) / (depthOfSlice(g, q.x) + 1 / g.k));
                if (q.x <= g.zFar) worstFar = std::max(worstFar, std::abs(a.z - farDepth(g, q.x)) / farDepth(g, q.x));
                worstTrip = std::max(worstTrip, std::abs(a.w - q.x) / (q.x + 1 / g.k));
                double za = q.y, zb = q.z;
                segment(za, zb, q.w);
                worstSegment = std::max({ worstSegment, std::abs(b.x - za), std::abs(b.y - zb) });
            }
            report(worstSlice <= 2e-4, "grid on the GPU: fogSliceOfDepth vs the twin (slices)", worstSlice, 2e-4);
            report(worstDepth <= 4e-6, "grid on the GPU: fogDepthOfSlice vs the twin (rel. to depth + 1 / k)", worstDepth, 4e-6);
            report(worstFar <= 4e-6, "grid on the GPU: fogFarDepth vs the twin (rel.)", worstFar, 4e-6);
            report(worstTrip <= 1e-5, "grid on the GPU: depth -> slice -> depth (rel. to depth + 1 / k)", worstTrip, 1e-5);
            report(worstSegment <= 2e-5, "grid on the GPU: fogSegment vs the twin (m)", worstSegment, 2e-5);
        }
        if (want("noise"))
        {
            const uint32_t count = (uint32_t)noiseQueries.size();
            const std::vector<float4> out = probe(2, noiseQueries, count, 1, noWords);
            double worstScale = 0, worstNoise = 0, worstLattice = 0;
            uint32_t periodDiffers = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float4 q = noiseQueries[i];
                worstScale = std::max(worstScale, (double)std::abs(out[i].x - densityScale(q.x, q.y, q.z, q.w)));
                worstNoise = std::max(worstNoise, (double)std::abs(out[i].y - valueNoise(q.x, q.y, q.z)));
                worstLattice = std::max(worstLattice, (double)std::abs(out[i].z - latticeValue((int32_t)std::floor(q.x), (int32_t)std::floor(q.y), (int32_t)std::floor(q.z))));
                if (i < 2048 && (i & 1) && (bitsOf(out[i].x) != bitsOf(out[i - 1].x) || bitsOf(out[i].y) != bitsOf(out[i - 1].y))) ++periodDiffers;
            }
            // (the hash's 16 bits are 3e-5 apart in the value: the same bits on both sides; the float's last place is the GPU's
            //  fused multiply-add)
            report(worstLattice <= 2.4e-7, "noise on the GPU: fogLatticeValue vs the twin (the same hash: 2 ulp of the float)", worstLattice, 2.4e-7);
            report(worstNoise <= 2e-5, "noise on the GPU: fogValueNoise vs the twin", worstNoise, 2e-5);
            report(worstScale <= 2e-5, "noise on the GPU: fogDensityScale vs the twin", worstScale, 2e-5);
            report(periodDiffers == 0, "noise on the GPU: the same bits 256 lattice points on (pairs differing)", periodDiffers, 0);
        }

        // ---- 9. The views' rays far from the render origin.
        if (want("rays"))
        {
            struct Place
            {
                const char* name;
                float3 position;
            };
            const Place places[3] = { { "the camera at the render origin", { 3, 1.7f, -2 } },
                                      { "the camera 1,000 m from the render origin", { 800, 1.7f, -600 } },
                                      { "the camera 10,000 m from the render origin", { -6000, 250, 8000 } } };
            auto cross3 = [](D3 a, D3 b) { return D3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };
            for (const Place& place : places)
            {
                scene::Camera cam = open.cameras[0];
                cam.position = place.position;
                cam.forward = normalize(float3{ 0.37f, -0.21f, 0.9f });
                const ViewDesc view = ViewDesc::fromCamera(cam, W, H, float4x4{});
                // the camera's basis and its pixels' rays in double (Math.h lookTo, perspectiveReversedInfinite)
                const D3 forward = ref::normalize(d3(cam.forward)), right = ref::normalize(cross3(d3(cam.up), forward * -1.0)), up = cross3(forward * -1.0, right);
                const double tanHalf = std::tan((double)cam.verticalFov * 0.5), aspect = (double)W / H, pixelAngle = 2 * tanHalf / H;
                auto mainRay = [&](double px, double py) { return right * ((px / W * 2 - 1) * aspect * tanHalf) + up * ((1 - py / H * 2) * tanHalf) + forward; };
                // a mirror 3 m under the camera, tilted; the reflection view of a part of the main view
                const D3 n = ref::normalize(D3{ 0.2, 1.0, 0.1 });
                const double offset = -(ref::dot(n, d3(cam.position)) - 3.0);
                const uint32_t rx = W / 4, ry = H / 3, rw = W / 2, rh = H / 2;
                const ViewDesc mirror = ViewDesc::planarReflection(view, float4{ (float)n.x, (float)n.y, (float)n.z, (float)offset }, rx, ry, rw, rh);
                for (int which = 0; which < 2; ++which)
                {
                    const uint32_t w = which == 0 ? W : rw, h = which == 0 ? H : rh;
                    std::vector<float4> queries;
                    for (uint32_t iy = 0; iy <= 12; ++iy)
                        for (uint32_t ix = 0; ix <= 16; ++ix) queries.push_back({ (float)(w * ix / 16.0), (float)(h * iy / 12.0), 0, 0 });
                    for (int i = 0; i < 600; ++i) queries.push_back({ (float)uni(0, w), (float)uni(0, h), 0, 0 });
                    const uint32_t count = (uint32_t)queries.size();
                    std::shared_ptr<std::vector<uint8_t>> rb;
                    tf.fogParams = 0;
                    const ViewDesc& v = which == 0 ? view : mirror;
                    tf.run([&](FramePassContext& fc) { rb = probePass(tf, fc, 5, queries, count, 1, fc.frameConstantsFor(v), noWords, TextureRef{}, TextureRef{}); });
                    const std::vector<float4> out = floatsOf(*rb);
                    double worst = 0, worstDepth = 0;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        D3 want0 = which == 0 ? mainRay(queries[i].x, queries[i].y) : mainRay(rx + (double)queries[i].x, ry + (double)queries[i].y);
                        if (which == 1) want0 = want0 - n * (2 * ref::dot(n, want0));  // (the mirror's view: the main view's ray reflected)
                        const D3 got{ out[i].x, out[i].y, out[i].z };
                        const D3 axis = which == 0 ? forward : forward - n * (2 * ref::dot(n, forward));
                        worst = std::max(worst, len(got - want0) / pixelAngle);
                        worstDepth = std::max(worstDepth, std::abs(ref::dot(got, axis) - 1.0));
                    }
                    const std::string name = std::string(place.name) + (which == 0 ? ", the main view" : ", a cropped planar reflection view");
                    logf("%s: %u rays, largest error %.4g pixels, view depth off 1 by up to %.3g\n", name.c_str(), count, worst, worstDepth);
                    report(worst <= 0.01, name + ": froxelRayAt vs the camera's ray in double (pixels)", worst, 0.01);
                }
            }
        }

        const std::array<float, 3> white = { 1, 1, 1 };
        tf.frame.fogVolumes.clear();

        // ---- 3. A uniform medium in a frame.
        if (want("uniform"))
        {
            // thin (the default density: its fp16 value is not the float's), history off and on
            setFog(0.002, 0, 0, 0, white);
            Options o;
            o.columns = spread;
            const Result off = run(open, o);
            if (volumeThere(off, "uniform 0.002"))
            {
                const FogDesc& fd = tf.frame.fog;
                report(off.fog.density == fd.density && off.fog.falloff == fd.heightFalloff && off.fog.g == fd.phaseG && off.fog.start == fd.startDistance,
                       "uniform 0.002: the frame's medium is the volume's (density)", off.fog.density, fd.density);
                againstTwin(off, spread, "uniform 0.002, history off");
                // the formula itself with one source per column: S = albedo E T_sun / (4 pi) at the column's middle cell
                double worstS = 0, worstLiteral = 0, sunChange = 0;
                for (size_t i = 0; i < spread.size(); ++i)
                {
                    const double toRay = len(off.model.view.rayAt((spread[i].first + 0.5) * grid.cellPx, (spread[i].second + 0.5) * grid.cellPx));
                    const float4 mid = off.sun[i * perColumn + gz / 2];
                    for (uint32_t z = 0; z < gz; ++z)
                    {
                        sunChange = std::max(sunChange, (double)std::abs(off.sun[i * perColumn + z].z / mid.z - 1));
                        const double d = depthOfSlice(grid, z + 1.0) * toRay, opacity = 1 - std::exp(-0.002f * d);
                        const std::array<double, 4> g = off.volume.at(spread[i].first, spread[i].second, z);
                        const double S[3] = { off.model.E[0] * mid.x / (4 * kPi), off.model.E[1] * mid.y / (4 * kPi), off.model.E[2] * mid.z / (4 * kPi) };
                        for (int k = 0; k < 3; ++k) worstLiteral = std::max(worstLiteral, std::abs(g[k] - S[k] * opacity) / (S[k] * opacity));
                        worstS = std::max(worstS, S[2]);
                    }
                }
                // (the references' sun is the GPU's table lookup: here against the atmosphere's own reference, within the
                //  table's 2e-3 - FroxelTests 4)
                const ref::Model air = ref::fromScene(open.atmosphere);
                double worstSun = 0;
                for (size_t i = 0; i < spread.size(); i += 3)
                    for (uint32_t z : { 0u, gz / 2, gz - 1 })
                    {
                        D3 p = samplePoint(off.model, spread[i].first, spread[i].second, z, off.model.samples[0]);
                        if (ref::altitudeOf(air, p) < 0) p = p + ref::upOf(air, p) * -ref::altitudeOf(air, p);  // (under the model's surface: the surface's air)
                        const D3 exact = ref::sunTransmittance(air, p, off.model.sun, 2048);
                        const float4 got = off.sun[i * perColumn + z];
                        worstSun = std::max({ worstSun, std::abs(got.x - exact.x) / exact.x, std::abs(got.y - exact.y) / exact.y, std::abs(got.z - exact.z) / exact.z });
                    }
                report(worstSun <= 2e-3, "uniform 0.002: the sun the references take vs the atmosphere reference (rel.)", worstSun, 2e-3);
                const double literal = kUlp * 4 + 2e-5 + sunChange;
                logf("uniform 0.002: S up to %.5g nits; the sun's transmittance changes by %.2e along a column's cells\n", worstS, sunChange);
                report(worstLiteral <= literal, "uniform 0.002: L = S (1 - e^(-sigma d)) with one S per column (rel.; storage + the sun's change)", worstLiteral, literal);
            }
            o.history = true;
            o.frames = 24;
            const Result on = run(open, o);
            if (volumeThere(on, "uniform 0.002, history"))
            {
                const std::vector<double> sunOverCell(1, 5e-4);  // the sun's transmittance over a cell's extent (under 1.5e-4 per metre of height)
                againstTwin(on, spread, "uniform 0.002, history on (24 frames)", &sunOverCell);
            }
            // dense, coloured
            setFog(1.0 / 32, 0, 0, 0, { 0.9f, 0.6f, 0.3f });
            o.history = false;
            o.frames = 2;
            const Result dense = run(open, o);
            if (volumeThere(dense, "uniform 1/32")) againstTwin(dense, spread, "uniform 1/32, albedo (0.9, 0.6, 0.3)");
            // a phase function toward a low sun inside the view (20 klx: the radiance toward it stays inside fp16's range)
            scene::Scene low = open;
            low.sun.direction = normalize(float3{ 0.25f, 0.3f, 1 });
            low.sun.illuminance = 20000;
            setFog(0.01, 0, 0, 0.6, white);
            const Result phase = run(low, o);
            if (volumeThere(phase, "uniform 0.01, g 0.6")) againstTwin(phase, spread, "uniform 0.01, g 0.6, a 20 klx sun in the view");
            // strong forward scattering toward the full sun in a dense medium: the radiance passes 65504 nits (fp16's largest
            // value, where the volume was cut while it held nits)
            low.sun.illuminance = 128000;
            setFog(0.2, 0, 0, 0.8, white);
            const Result strong = run(low, o);
            if (volumeThere(strong, "uniform 0.2, g 0.8"))
            {
                uint32_t notFinite = 0, texels = 0, past = 0;
                double largest = 0, largestRef = 0;
                for (uint32_t z = 0; z < total; ++z)
                    for (uint32_t cy = 0; cy < grid.y; ++cy)
                        for (uint32_t cx = 0; cx < grid.x; ++cx)
                        {
                            const std::array<double, 4> g = strong.volume.at(cx, cy, z);
                            ++texels;
                            if (!std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2]) || !std::isfinite(g[3])) ++notFinite;
                            else largest = std::max({ largest, g[0], g[1], g[2] });
                        }
                Tally within;
                for (size_t i = 0; i < spread.size(); ++i)
                {
                    const Column c = twinColumn(strong.model, spread[i].first, spread[i].second, &strong.sun[i * perColumn]);
                    largestRef = std::max({ largestRef, c.L[total - 1][0], c.L[total - 1][1], c.L[total - 1][2] });
                    compareFaces(strong.volume, spread[i].first, spread[i].second, c, 0, total, nullptr, within, "uniform 0.2, g 0.8");
                    for (uint32_t z = 0; z < total; ++z)
                        for (int k = 0; k < 3; ++k) past += c.L[z][k] > kHalfMax ? 1 : 0;
                }
                logf("uniform 0.2, g 0.8, 128 klx: the reference's largest radiance over the compared columns %.5g nits; the volume's largest %.5g nits (%.4g exposed); "
                     "%u compared values are past 65504 nits\n", largestRef, largest, largest * strong.exposure, past);
                report(notFinite == 0, "uniform 0.2, g 0.8: texels of the volume that are not finite (of " + std::to_string(texels) + ")", notFinite, 0);
                report(past > 1000 && within.over == 0, "uniform 0.2, g 0.8: every column, those past 65504 nits toward the sun too, vs the twin (faces outside the tolerance)",
                       within.over, 0);
                report(largest >= 0.99 * largestRef, "uniform 0.2, g 0.8: the volume holds the radiance past 65504 nits (its largest, nits)", largest, 0.99 * largestRef);
            }
            // the density's variation in a frame: drifting in still air, then with the scene's wind
            setFog(0.01, 0.02, 0, 0, white, 0, 0.3, 20);
            tf.frame.time = 123.4;
            const Result still = run(open, o);
            if (volumeThere(still, "noise 0.3")) againstTwin(still, spread, "noise 0.3 over 20 m, drift in still air");
            scene::Scene windy = open;
            windy.windDirection = normalize(float3{ 0.6f, 0, 0.8f });
            windy.windSpeed = 2.5f;
            const Result blown = run(windy, o);
            if (volumeThere(blown, "noise 0.3, wind")) againstTwin(blown, spread, "noise 0.3 over 20 m, wind 2.5 m/s");
            tf.frame.time = 0;
        }

        // ---- 2 and 5 in a frame: the readers.
        if (want("reader"))
        {
            setFog(0.002, 0, 0, 0, white);
            Options o;
            o.columns = { { grid.x / 2, grid.y / 2 }, { grid.x / 5, grid.y / 3 }, { 4 * grid.x / 5, 2 * grid.y / 3 }, { 2, 3 }, { grid.x - 3, grid.y - 4 } };
            o.debugView = true;
            const ViewCpu view{ tf.frame.mainView, d3(tf.frame.mainView.position) };
            const double depths[25] = { 0.03, 0.0928, 0.5, 2, 7.3, 25, 60, 79, 79.9, 79.99, 79.999, 80, 80.001, 80.01, 80.1, 81, 95, 121.7, 300, 2000, 30000, 65536, 1e6, 3e38, 12.5 };
            const uint32_t centreQueries = (uint32_t)o.columns.size() * 25;
            auto pushQuery = [&](double u, double v, double depth) {
                const D3 ray = view.rayAt(u * W, v * H), origin = view.cam + ray * std::min(depth, 1e5);
                const double cz = uni(-1, 1), az = uni(0, 2 * kPi), cr = std::sqrt(1 - cz * cz);
                o.reader.push_back({ (float)u, (float)v, (float)depth, (float)(uni(0, 1) < 0.2 ? 65536.0 : std::exp(uni(std::log(0.5), std::log(3000.0)))) });
                o.reader.push_back({ (float)origin.x, (float)origin.y, (float)origin.z, 0 });
                o.reader.push_back({ (float)(cr * std::cos(az)), (float)cz, (float)(cr * std::sin(az)), 0 });
            };
            for (const auto& c : o.columns)
                for (double depth : depths) pushQuery((c.first + 0.5) * grid.cellPx / W, (c.second + 0.5) * grid.cellPx / H, depth);
            for (int i = 0; i < 1500; ++i) pushQuery(uni(0, 1), uni(0, 1), std::exp(uni(std::log(0.02), std::log(2e5))));
            const uint32_t queries = (uint32_t)(o.reader.size() / 3);
            const Result r = run(open, o);
            if (volumeThere(r, "reader"))
            {
                const double farScale = grid.zFar / std::log2(grid.farEndM / grid.farM);
                double worstFilter = 0, worstExactT = 0, worstExactL = 0, interpNearT = 0, interpFarT = 0, interpNearL = 0, interpFarL = 0, worstAir = 0, worstOther = 0, worstRay = 0, worstSky = 0;
                uint32_t overFilter = 0, overExact = 0, raysFogged = 0, raysLeft = 0;
                std::vector<Column> columns;
                for (size_t i = 0; i < o.columns.size(); ++i) columns.push_back(twinColumn(r.model, o.columns[i].first, o.columns[i].second, &r.sun[i * perColumn]));
                Medium mean = r.model.medium;  // fogOverRay's medium: no start distance
                mean.start = 0;
                for (uint32_t i = 0; i < queries; ++i)
                {
                    const float4 q = o.reader[3 * i], qo = o.reader[3 * i + 1], qd = o.reader[3 * i + 2];
                    const float4 at = r.reader[6 * i], other = r.reader[6 * i + 1], air = r.reader[6 * i + 2], airT = r.reader[6 * i + 3], ray = r.reader[6 * i + 4], sky = r.reader[6 * i + 5];
                    const double got[4] = { at.x, at.y, at.z, at.w };
                    // (a) the reader against the volume read back
                    std::array<double, 4> range{};
                    const std::array<double, 4> expect = readerAt(r.volume, grid, W, H, q.x, q.y, q.z, &range);
                    bool bad = false;
                    for (int k = 0; k < 4; ++k)
                    {
                        const double tol = 3.0 / 256 * range[k] + (kUlp + 1e-5) * std::abs(expect[k]) + 1e-7, e = std::abs(got[k] - expect[k]);
                        worstFilter = std::max(worstFilter, e / std::max(std::abs(expect[k]), 1e-3));
                        bad = bad || !(e <= tol);
                    }
                    if (bad && overFilter++ < 5)
                        logf("  reader: uv (%.4f, %.4f) depth %g: fogAt %.6g %.6g %.6g %.6g, the volume read back gives %.6g %.6g %.6g %.6g\n", (double)q.x, (double)q.y, (double)q.z, got[0],
                             got[1], got[2], got[3], expect[0], expect[1], expect[2], expect[3]);
                    // (b) at cell centres: against the exact medium, with the interpolation's own error
                    if (i < centreQueries)
                    {
                        const size_t ci = i / 25;
                        const Column& c = columns[ci];
                        const double toRay = len(view.rayAt(q.x * W, q.y * H)), sigma = r.model.medium.density;
                        const double depth = std::min((double)q.z, grid.farEndM), coord = std::min(readerCoord(grid, depth), (double)total);
                        const uint32_t n = std::min((uint32_t)coord, total - 1);  // the slice the depth is in
                        const double before = n == 0 ? 0.0 : (n <= gz ? depthOfSlice(grid, n) : farDepth(grid, n - gz));
                        const double T0 = n == 0 ? 1.0 : c.T[n - 1], e = std::exp(-sigma * (depth - before) * toRay);
                        const float4 sunT = r.sun[ci * perColumn + std::min(n, gz)];
                        const double S[3] = { r.model.E[0] * sunT.x / (4 * kPi), r.model.E[1] * sunT.y / (4 * kPi), r.model.E[2] * sunT.z / (4 * kPi) };
                        const double tau = (n == 0 ? 0.0 : c.tau[n - 1]) + sigma * (depth - before) * toRay, frac = coord - n;
                        const double exactT = T0 * e, linearT = T0 + (c.T[n] - T0) * frac;
                        const double interpT = std::abs(linearT - exactT);
                        (n < gz ? interpNearT : interpFarT) = std::max(n < gz ? interpNearT : interpFarT, interpT);
                        const double filter = 3.0 / 256 * range[3];
                        const double tolT = interpT + filter + (kUlp * (2 + tau) + 2e-5) * exactT + 1e-7;  // (storage, and the filter's result)
                        bool off = !(std::abs(got[3] - exactT) <= tolT);
                        worstExactT = std::max(worstExactT, std::abs(got[3] - exactT));
                        for (int k = 0; k < 3; ++k)
                        {
                            const double L0 = n == 0 ? 0.0 : c.L[n - 1][k], exactL = L0 + T0 * S[k] * (1 - e), linearL = L0 + (c.L[n][k] - L0) * frac;
                            const double interpL = std::abs(linearL - exactL);
                            (n < gz ? interpNearL : interpFarL) = std::max(n < gz ? interpNearL : interpFarL, interpL / std::max(exactL, 1e-3));
                            const double tolL = interpL + 3.0 / 256 * range[k] + (kUlp * (4 + tau) + 2e-5) * exactL + 1e-6;
                            off = off || !(std::abs(got[k] - exactL) <= tolL);
                            worstExactL = std::max(worstExactL, std::abs(got[k] - exactL) / std::max(exactL, 1e-3));
                        }
                        if (off && overExact++ < 5)
                            logf("  reader vs the medium: cell (%u, %u) depth %g: fogAt T %.6g, exact %.6g (interpolation %.2e)\n", o.columns[ci].first, o.columns[ci].second,
                                 (double)q.z, got[3], exactT, interpT);
                    }
                    // (c) fogVolumeAt, fogOverAir, fogOverSky, fogOverRay from fogAt's own value
                    for (int k = 0; k < 4; ++k)
                    {
                        const double o2[4] = { other.x, other.y, other.z, other.w };
                        worstOther = std::max(worstOther, (std::abs(o2[k] - got[k]) - 1.0 / 256 * range[k]) / std::max(std::abs(got[k]), 1e-3));
                    }
                    const double in0[3] = { 1, 2, 3 }, tr0[3] = { 0.5, 0.6, 0.7 }, radiance[3] = { 100, 200, 300 }, airGot[3] = { air.x, air.y, air.z }, trGot[3] = { airT.x, airT.y, airT.z };
                    for (int k = 0; k < 3; ++k)
                        worstAir = std::max({ worstAir, std::abs(airGot[k] - (in0[k] * got[3] + got[k])) / (in0[k] * got[3] + got[k]), std::abs(trGot[k] - tr0[k] * got[3]) / std::max(tr0[k] * got[3], 1e-6) });
                    std::array<double, 4> skyRange{};
                    const std::array<double, 4> whole = readerAt(r.volume, grid, W, H, q.x, q.y, 3.0e38, &skyRange);
                    const double skyGot[3] = { sky.x, sky.y, sky.z }, rayGot[3] = { ray.x, ray.y, ray.z };
                    for (int k = 0; k < 3; ++k)
                    {
                        const double wantSky = radiance[k] * whole[3] + whole[k];  // sky amount 1
                        worstSky = std::max(worstSky, (std::abs(skyGot[k] - wantSky) - 3.0 / 256 * (skyRange[k] + radiance[k] * skyRange[3])) / wantSky - kUlp);
                    }
                    const double T = std::exp(-opticalDepthClosed(mean, D3{ qo.x, qo.y, qo.z }, D3{ qd.x, qd.y, qd.z }, 0.0, q.w)), opacity = 1 - got[3];
                    const bool left = T > 0.999 || opacity < 4e-3;
                    // (near the two thresholds the float's side decides: those queries are not judged)
                    if (std::abs(T - 0.999) > 1e-5 && std::abs(opacity - 4e-3) > 1e-5)
                    {
                        (left ? raysLeft : raysFogged)++;
                        for (int k = 0; k < 3; ++k)
                        {
                            const double wantRay = left ? radiance[k] : radiance[k] * T + got[k] * ((1 - T) / opacity);
                            worstRay = std::max(worstRay, std::abs(rayGot[k] - wantRay) / wantRay);
                        }
                    }
                    if (i == 0) report(sky.w == 1.0f, "reader: the record carries sky_amount (1 by default)", sky.w, 1);
                }
                logf("reader: %u queries (%u at cell centres); the reader's interpolation between faces: T up to %.2e in the cells, %.2e in the far slices; L up to %.2e, %.2e (rel.)\n",
                     queries, centreQueries, interpNearT, interpFarT, interpNearL, interpFarL);
                logf("reader: fogAt vs the volume read back, worst rel. %.3e; vs the exact medium at cell centres: T %.3e, L %.3e (rel.); farScale %.5f\n", worstFilter, worstExactT,
                     worstExactL, farScale);
                report(overFilter == 0, "reader: fogAt vs the hardware's fetch of the volume read back (queries outside 3/256 of the texels' spread + 2^-10)", overFilter, 0);
                report(overExact == 0, "reader: fogAt vs the exact medium at cell centres (outside interpolation + storage + filter)", overExact, 0);
                report(worstOther <= 1e-5, "reader: fogVolumeAt agrees with fogAt (rel., past one filter step)", worstOther, 1e-5);
                report(worstAir <= 1e-6, "reader: fogOverAir = in-scattering x T + L, transmittance x T (rel.)", worstAir, 1e-6);
                report(worstSky <= 1e-5, "reader: fogOverSky at sky_amount 1 = radiance x T + L over the whole ray (rel., past the filter and its precision)", worstSky, 1e-5);
                report(raysFogged > 100 && raysLeft > 100 && worstRay <= 1e-3, "reader: fogOverRay = radiance x T_ray + L (1 - T_ray) / opacity, or left alone (rel.)", worstRay, 1e-3);
                // no step at the cells' end: just before and just past volumetric_distance_m
                double worstStep = 0, worstRise = 0;
                for (size_t ci = 0; ci < o.columns.size(); ++ci)
                {
                    const uint32_t cx = o.columns[ci].first, cy = o.columns[ci].second;
                    const double toRay = len(view.rayAt((cx + 0.5) * grid.cellPx, (cy + 0.5) * grid.cellPx));
                    const double a0 = r.volume.at(cx, cy, gz - 2)[3], a1 = r.volume.at(cx, cy, gz - 1)[3], a2 = r.volume.at(cx, cy, gz)[3];
                    for (int k = 7; k < 16; ++k) worstRise = std::max(worstRise, (double)r.reader[6 * (ci * 25 + k + 1)].w - (double)r.reader[6 * (ci * 25 + k)].w);
                    for (int pair = 0; pair < 3; ++pair)  // 79.9 | 80.1, 79.99 | 80.01, 79.999 | 80.001
                    {
                        const double before = r.reader[6 * (ci * 25 + 8 + pair)].w, after = r.reader[6 * (ci * 25 + 14 - pair)].w, eps = pair == 0 ? 0.1 : (pair == 1 ? 0.01 : 0.001);
                        // what the medium removes over 2 eps (twice over: the two sides' slopes differ) and one step of the filter's weights
                        const double allowed = 2 * 0.002 * 2 * eps * toRay * a1 + (std::abs(a1 - a0) + std::abs(a2 - a1)) / 256 + 1e-6;
                        worstStep = std::max(worstStep, (before - after) / allowed);
                    }
                }
                report(worstStep <= 1 && worstStep >= -1, "reader: no step in the transmittance across the cells' end (share of the allowed change)", worstStep, 1);
                report(worstRise <= 1e-6, "reader: the transmittance does not rise with depth from 79 m to 81 m", worstRise, 1e-6);
                // the debug view: fogAt x exposure at the pixel (sky pixels: the whole ray)
                if (!r.debug.empty())
                {
                    const double exposure = 1.0 / (1.2 * std::exp2((double)tf.frame.mainView.ev100));
                    const size_t pitch = TestFrame::rowPitch(W, 8);
                    double worstDebug = 0;
                    for (int i = 0; i < 400; ++i)
                    {
                        const uint32_t px = (uint32_t)uni(0, W - 1), py = (uint32_t)uni(0, H - 1);
                        uint16_t h[4];
                        std::memcpy(h, r.debug.data() + py * pitch + (size_t)px * 8, 8);
                        std::array<double, 4> range{};
                        const std::array<double, 4> whole = readerAt(r.volume, grid, W, H, (px + 0.5) / W, (py + 0.5) / H, 3.0e38, &range);
                        for (int k = 0; k < 4; ++k)
                        {
                            const double wantD = k < 3 ? whole[k] * exposure : whole[k], gotD = halfToFloat(h[k]), filter = 3.0 / 256 * range[k] * (k < 3 ? exposure : 1.0);
                            worstDebug = std::max(worstDebug, (std::abs(gotD - wantD) - filter) / std::max(wantD, 1e-6));
                        }
                    }
                    // (the filter's result and the image's own texel: fp16 both)
                    report(worstDebug <= 2 * kUlp + 2e-5, "reader: the debug view holds fogAt x exposure and the transmittance (rel., past the filter)", worstDebug, 2 * kUlp + 2e-5);
                }
                else report(false, "reader: atmosphere.fog.debug_view gives the image", 0, 1);
            }
            // sky_amount 0 and 0.5; reflection rays without the fog (on_rays off)
            Options sky;
            sky.reader.assign(o.reader.begin(), o.reader.begin() + 3 * 400);
            for (float amount : { 0.0f, 0.5f })
            {
                tf.frame.fog.skyAmount = amount;
                const Result s = run(open, sky);
                double worst = 0;
                if (volumeThere(s, "sky amount"))
                    for (uint32_t i = 0; i < 400; ++i)
                    {
                        const float4 q = sky.reader[3 * i], got = s.reader[6 * i + 5];
                        std::array<double, 4> range{};
                        const std::array<double, 4> whole = readerAt(s.volume, grid, W, H, q.x, q.y, 3.0e38, &range);
                        const double radiance[3] = { 100, 200, 300 }, g3[3] = { got.x, got.y, got.z };
                        for (int k = 0; k < 3; ++k)
                        {
                            const double through = radiance[k] * whole[3] + whole[k], wantSky = radiance[k] + (through - radiance[k]) * amount;
                            worst = std::max(worst, (std::abs(g3[k] - wantSky) - amount * (3.0 / 256 * (range[k] + radiance[k] * range[3]) + kUlp * through)) / wantSky);
                        }
                        if (got.w != amount) worst = 1;
                    }
                report(worst <= (amount == 0 ? 0.0 : 1e-5), format("sky_amount %.1f: fogOverSky takes that share of the fog (rel.; 0: the sky's bits)", amount), worst, amount == 0 ? 0.0 : 1e-5);
            }
            tf.frame.fog.skyAmount = 1;
            tf.quality.applyOverride("atmosphere.fog.on_rays=false");
            {
                const Result s = run(open, sky);
                uint32_t changed = 0;
                for (uint32_t i = 0; i < 400 && s.reader.size() >= 6 * 400; ++i)
                    changed += s.reader[6 * i + 4].x != 100.0f || s.reader[6 * i + 4].y != 200.0f || s.reader[6 * i + 4].z != 300.0f ? 1 : 0;
                report(s.reader.size() >= 6 * 400 && changed == 0, "on_rays off: fogOverRay leaves the rays' radiance (rays changed)", changed, 0);
            }
            tf.quality.applyOverride("atmosphere.fog.on_rays=true");
            // the fog off: no record, no volume, the readers give nothing
            tf.frame.fog = FogDesc{};
            {
                const Result s = run(open, sky);
                uint32_t changed = 0;
                for (uint32_t i = 0; i < 400 && s.reader.size() >= 6 * 400; ++i)
                {
                    const float4 at = s.reader[6 * i], air = s.reader[6 * i + 2], ray = s.reader[6 * i + 4], skyOut = s.reader[6 * i + 5];
                    changed += at.x != 0 || at.y != 0 || at.z != 0 || at.w != 1 || air.x != 1 || air.y != 2 || air.z != 3 || ray.x != 100 || skyOut.x != 100 || skyOut.w != -1 ? 1 : 0;
                }
                report(s.word == 0 && !s.hasVolume && s.reader.size() >= 6 * 400 && changed == 0, "fog off: no record, no volume, the readers leave their inputs (queries changed)", changed, 0);
            }
        }

        // ---- 4. Height falloff in a frame: columns looking up and down.
        if (want("falloff"))
        {
            scene::Scene level = open;
            level.cameras[0].position = { 0, 20, 0 };
            level.cameras[0].forward = { 0, 0, 1 };
            for (int config = 0; config < 2; ++config)
            {
                // 0: the default falloff, the camera at the fog's height; 1: steep, the camera 20 m under the fog's height, the
                // bottom rows reach the height where the density is held
                const double falloff = config == 0 ? 0.02 : 0.1, height = config == 0 ? 20 : 40, density = config == 0 ? 0.01 : 0.001;
                const std::string name = format("falloff %.2f", falloff);
                setFog(density, falloff, height, 0, white);
                Options o;
                o.columns = spread;
                const Result off = run(level, o);
                if (!volumeThere(off, name)) continue;
                againstTwin(off, spread, name + ", history off: the twin at the frame's sample points");
                o.history = true;
                o.frames = 40;
                const Result on = run(level, o);
                if (!volumeThere(on, name + ", history")) continue;
                const std::vector<double> rounding(1, kHistoryRounding + 5e-4);  // (and the sun over a cell: taken at its centre)
                againstTwin(on, spread, name + ", history on (40 frames): the twin of the frames' samples", &rounding);
                // what the storage's cut costs: the optical depth and the radiance at the cells' end against the twins, signed,
                // over the columns with an optical depth of at least 0.2 there (the transmittance's own fp16 step is small)
                for (int pass = 0; pass < 2; ++pass)
                {
                    const Result& r = pass == 0 ? off : on;
                    double sumTau = 0, sumL = 0;
                    uint32_t n = 0;
                    for (size_t i = 0; i < spread.size(); ++i)
                    {
                        const Column c = twinColumn(r.model, spread[i].first, spread[i].second, &r.sun[i * perColumn]);
                        const std::array<double, 4> g = r.volume.at(spread[i].first, spread[i].second, gz - 1);
                        if (c.tau[gz - 1] < 0.2 || g[3] <= 0) continue;
                        sumTau += -std::log(g[3]) / c.tau[gz - 1] - 1;
                        sumL += g[1] / c.L[gz - 1][1] - 1;
                        ++n;
                    }
                    logf("%s, history %s: at the cells' end the volume's optical depth is %+.3f %% and its radiance %+.3f %% of the twin's (mean of %u columns)\n", name.c_str(),
                         pass == 0 ? "off" : "on", 100 * sumTau / std::max(n, 1u), 100 * sumL / std::max(n, 1u), n);
                }
                // the closed form along the centre rays, within the cells' bound
                for (int pass = 0; pass < 2; ++pass)
                {
                    const Result& r = pass == 0 ? off : on;
                    Tally up, down;
                    double largestBound = 0, heldCells = 0;
                    for (size_t i = 0; i < spread.size(); ++i)
                    {
                        const D3 ray = r.model.view.rayAt((spread[i].first + 0.5) * grid.cellPx, (spread[i].second + 0.5) * grid.cellPx);
                        const Column c = closedColumn(r.model, spread[i].first, spread[i].second, &r.sun[i * perColumn]);
                        std::vector<double> bound = cellBound(r.model, spread[i].first, spread[i].second);
                        for (double& b : bound) b += pass == 1 ? kHistoryRounding + 5e-4 : 0.0;
                        largestBound = std::max(largestBound, bound.back());
                        if (r.model.view.cam.y + ray.y * grid.farM < height - 6 / falloff) ++heldCells;
                        compareFaces(r.volume, spread[i].first, spread[i].second, c, 0, total, &bound, ray.y > 0 ? up : down, name.c_str());
                    }
                    const std::string label = name + (pass == 0 ? ", history off" : ", history on");
                    logf("%s vs the closed form: looking up %u faces, worst rel. L %.3e T %.3e; looking down %u faces, L %.3e T %.3e; the cells' bound up to %.3f; %g columns reach the held density\n",
                         label.c_str(), up.faces, up.worstL, up.worstT, down.faces, down.worstL, down.worstT, largestBound, heldCells);
                    report(up.faces > 0 && up.over == 0, label + ": closed form, columns looking up (faces outside the cells' bound)", up.over, 0);
                    report(down.faces > 0 && down.over == 0, label + ": closed form, columns looking down (faces outside the cells' bound)", down.over, 0);
                }
            }
        }

        // ---- 10. The history across an exposure change: the same 40 frames with and without it.
        if (want("exposure"))
        {
            scene::Scene level = open;
            level.cameras[0].position = { 0, 20, 0 };
            level.cameras[0].forward = { 0, 0, 1 };
            setFog(0.01, 0.02, 20, 0, { 0.9f, 0.7f, 0.5f });
            Options o;
            o.columns = spread;
            o.history = true;
            o.frames = 40;
            o.startPhase = 3;
            const Result same = run(level, o);
            struct Change
            {
                const char* name;
                int frames;
                float ev;
                double limit;
            };
            // (a frame's cut differs between the two runs by 2^-10 of the cells' values and of the volume's; a power of two
            //  moves the exponents alone)
            const Change changes[4] = { { "the last frame 4 stops up", 1, 10.0f, 3 * kUlp },
                                        { "the last frame 2.6 stops up", 1, 11.4f, 3 * kUlp },
                                        { "the last frame 3.3 stops down", 1, 17.3f, 3 * kUlp },
                                        { "the last 12 frames 2.6 stops up", 12, 11.4f, kHistoryRounding + 2 * kUlp } };
            if (volumeThere(same, "exposure: no change"))
            {
                const std::vector<double> rounding(1, kHistoryRounding + 5e-4);
                againstTwin(same, spread, "exposure: 40 frames at EV 14, history on", &rounding);
                for (const Change& c : changes)
                {
                    Options oc = o;
                    oc.evFrames = c.frames;
                    oc.evThen = c.ev;
                    // the readers in the changed frame: fogAt (nits) against the volume read back
                    for (int i = 0; i < 200; ++i)
                    {
                        oc.reader.push_back({ (float)uni(0, 1), (float)uni(0, 1), (float)std::exp(uni(std::log(0.05), std::log(1e5))), 1 });
                        oc.reader.push_back({ 0, 0, 0, 0 });
                        oc.reader.push_back({ 0, 1, 0, 0 });
                    }
                    const Result r = run(level, oc);
                    const std::string name = std::string("exposure: ") + c.name;
                    if (!volumeThere(r, name)) continue;
                    report(r.model.samples.size() == same.model.samples.size() && r.exposure != same.exposure, name + ": the same frames at another exposure (ratio of the exposures)",
                           r.exposure / same.exposure, std::exp2(14.0 - c.ev));
                    againstTwin(r, spread, name + ": the twin", &rounding);
                    // against the run without the change, in nits, in the compared columns (the twin's allowance for the
                    // denormals' cut, for both runs)
                    double worst = 0;
                    uint32_t faces = 0;
                    for (size_t i = 0; i < spread.size(); ++i)
                    {
                        const Column col = twinColumn(r.model, spread[i].first, spread[i].second, &r.sun[i * perColumn]);
                        for (uint32_t z = 0; z < total; ++z)
                        {
                            const std::array<double, 4> a = same.volume.at(spread[i].first, spread[i].second, z), b = r.volume.at(spread[i].first, spread[i].second, z);
                            for (int k = 0; k < 3; ++k)
                            {
                                const double allowed = 2 * col.cut[z][k] + kHalfStep * (1 / same.exposure + 1 / r.exposure);
                                worst = std::max(worst, (std::abs(a[k] - b[k]) - allowed) / std::max(a[k], 1e-9));
                            }
                            ++faces;
                        }
                    }
                    // every texel: the transmittance does not know the exposure; a power of two leaves the radiance's bits
                    uint64_t alphaDiffers = 0, mantissaDiffers = 0, compared = 0;
                    const double ratio = r.exposure / same.exposure;
                    const bool power = std::exp2(std::round(std::log2(ratio))) == ratio;
                    for (uint32_t z = 0; z < total; ++z)
                        for (uint32_t cy = 0; cy < grid.y; ++cy)
                            for (uint32_t cx = 0; cx < grid.x; ++cx)
                            {
                                uint16_t ha[4], hb[4];
                                std::memcpy(ha, same.volume.texel(cx, cy, z), 8);
                                std::memcpy(hb, r.volume.texel(cx, cy, z), 8);
                                alphaDiffers += ha[3] != hb[3] ? 1 : 0;
                                for (int k = 0; k < 3 && power; ++k)
                                {
                                    const double va = halfToFloat(ha[k]), vb = halfToFloat(hb[k]);
                                    if (va < kHalfNormal || vb < kHalfNormal || va > 4000 || vb > 4000) continue;  // (normal values inside the range in both)
                                    ++compared;
                                    mantissaDiffers += vb != va * ratio ? 1 : 0;
                                }
                            }
                    logf("%s: %u faces of the compared columns, largest relative difference from the run without the change %.3e (in nits)\n", name.c_str(), faces, worst);
                    report(worst <= c.limit, name + ": the radiance in nits is the unchanged run's (largest rel. difference)", worst, c.limit);
                    report(alphaDiffers == 0, name + ": the transmittance is the unchanged run's (texels whose bits differ)", (double)alphaDiffers, 0);
                    if (power) report(compared > 100000 && mantissaDiffers == 0, name + ": a power of two moves the exponent alone (values with other bits)", (double)mantissaDiffers, 0);
                    uint32_t overFilter = 0;
                    for (uint32_t i = 0; i < 200 && r.reader.size() >= 6 * 200; ++i)
                    {
                        const float4 q = oc.reader[3 * i], at = r.reader[6 * i];
                        std::array<double, 4> range{};
                        const std::array<double, 4> expect = readerAt(r.volume, grid, W, H, q.x, q.y, q.z, &range);
                        const double got[4] = { at.x, at.y, at.z, at.w };
                        bool bad = false;
                        for (int k = 0; k < 4; ++k) bad = bad || !(std::abs(got[k] - expect[k]) <= 3.0 / 256 * range[k] + (kUlp + 1e-5) * std::abs(expect[k]) + 1e-7);
                        overFilter += bad ? 1 : 0;
                    }
                    report(r.reader.size() >= 6 * 200 && overFilter == 0, name + ": fogAt gives the volume's nits (queries outside 3/256 of the texels' spread + 2^-10)", overFilter, 0);
                }
            }
        }

        // ---- 5. start_distance_m: in the cells and past them.
        if (want("start"))
        {
            for (double start : { 10.0, 100.0 })
            {
                const std::string name = format("start %.0f m", start);
                setFog(0.01, 0, 0, 0, white, start);
                Options o;
                o.columns = spread;
                const Result r = run(open, o);
                if (!volumeThere(r, name)) continue;
                againstTwin(r, spread, name + ": the twin");
                // the medium itself: T = e^(-sigma (distance - start)); the slice the start falls in is all or nothing
                double worst = 0, beforeStart = 0;
                for (const auto& c : spread)
                {
                    const double toRay = len(r.model.view.rayAt((c.first + 0.5) * grid.cellPx, (c.second + 0.5) * grid.cellPx));
                    double slab = 0;  // the optical depth of the cells' slice the start is in
                    for (uint32_t z = 0; z < gz; ++z)
                        if (depthOfSlice(grid, z) * toRay <= start && depthOfSlice(grid, z + 1.0) * toRay > start) slab = 0.01 * (depthOfSlice(grid, z + 1.0) - depthOfSlice(grid, z)) * toRay;
                    for (uint32_t z = 0; z < total; ++z)
                    {
                        const double depth = z < gz ? depthOfSlice(grid, z + 1.0) : farDepth(grid, z - gz + 1.0), tau = (double)0.01f * std::max(depth * toRay - start, 0.0);
                        const double T = r.volume.at(c.first, c.second, z)[3];
                        if (depthOfSlice(grid, std::min(z + 2.0, (double)gz)) * toRay < start && z < gz) beforeStart = std::max(beforeStart, std::abs(T - 1));
                        worst = std::max(worst, (std::abs(T - std::exp(-tau)) - 6e-8) / ((slab + kUlp * (1 + tau) + 2e-5) * std::exp(-tau)));
                    }
                }
                report(beforeStart == 0, name + ": no fog in the slices before the start (|T - 1|)", beforeStart, 0);
                report(worst <= 1, name + ": T = e^(-sigma (distance - start)) (share of one slice at the start + storage)", worst, 1);
            }
        }

        // ---- 5. A caster between the sun and the cells.
        if (want("shadow"))
        {
            scene::Scene sc;
            sc.name = "fog roof";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("far box", { 1, 1, 1 }));
            sc.meshes.push_back(boxMesh("roof", { 60, 0.5f, 40 }));
            sc.instances.push_back(instanceAt(0, { -3000, 1, -3000 }));
            sc.instances.push_back(instanceAt(1, { 0, 20, 70 }));  // over z 30 .. 110 m, 18 m above the camera
            const std::vector<Box> casters = { { { 0, 20, 70 }, { 60, 0.5f, 40 } } };
            sc.sun.direction = normalize(float3{ 0.15f, 1, 0.1f });
            scene::Camera c;
            c.name = "main";
            c.position = { 0, 2, 0 };
            c.forward = { 0, 0, 1 };
            sc.cameras.push_back(c);
            setFog(0.01, 0, 0, 0, white);
            Options o;
            o.frames = 6;  // (the pages of the roof's shadow settle)
            for (uint32_t cy = grid.y / 2; cy < grid.y; cy += 4)  // level and below: these columns do not see the roof
                for (uint32_t cx = 2; cx < grid.x; cx += 9) o.columns.push_back({ cx, cy });
            const Result r = run(sc, o);
            if (volumeThere(r, "roof"))
            {
                Tally lit, transmittance;
                uint32_t shadowedCells = 0, shadowedChanged = 0, litCells = 0, mixedCells = 0;
                for (size_t i = 0; i < o.columns.size(); ++i)
                {
                    const uint32_t cx = o.columns[i].first, cy = o.columns[i].second;
                    const double px = (cx + 0.5) * grid.cellPx, py = (cy + 0.5) * grid.cellPx;
                    // a cell's segment lies within one slice either side of it, whatever the frame's place in the slice; the
                    // shadow's edge on the pages is within a texel or two of the caster's (a texel: the cell's width): two
                    // slices and two cells of margin
                    std::vector<double> share(total, 1.0);
                    std::vector<int> cls(gz, 1);
                    uint32_t firstUnlit = gz;
                    for (uint32_t z = 0; z < gz; ++z)
                    {
                        cls[z] = litClass(r.model, casters, px, py, 2.0 * grid.cellPx, depthOfSlice(grid, std::max(z, 2u) - 2.0), depthOfSlice(grid, z + 3.0));
                        if (cls[z] != 1 && firstUnlit == gz) firstUnlit = z;
                        (cls[z] == 1 ? litCells : (cls[z] == 0 ? shadowedCells : mixedCells))++;
                    }
                    const Column twin = twinColumn(r.model, cx, cy, &r.sun[i * perColumn], &share);
                    compareFaces(r.volume, cx, cy, twin, 0, firstUnlit, nullptr, lit, "roof (lit cells)");
                    // the transmittance does not know the shadow
                    Column onlyT = twin;
                    for (uint32_t z = 0; z < gz; ++z) onlyT.L[z] = { r.volume.at(cx, cy, z)[0], r.volume.at(cx, cy, z)[1], r.volume.at(cx, cy, z)[2] };
                    compareFaces(r.volume, cx, cy, onlyT, 0, gz, nullptr, transmittance, "roof (transmittance)");
                    for (uint32_t z = 1; z < gz; ++z)
                        if (cls[z] == 0 && !r.volume.sameRadiance(cx, cy, z, z - 1)) ++shadowedChanged;
                }
                logf("roof: %u cells in the sun before the shadow (worst rel. L %.3e), %u cells in the shadow, %u at its boundary (not judged)\n", lit.faces, lit.worstL, shadowedCells, mixedCells);
                report(lit.faces > 500 && lit.over == 0, "roof: cells in the sun before the shadow, vs the twin (faces outside the tolerance)", lit.over, 0);
                report(shadowedCells > 500 && shadowedChanged == 0, "roof: cells in the casters' shadow scatter no sun (cells whose radiance changes)", shadowedChanged, 0);
                report(transmittance.over == 0, "roof: the transmittance is the medium's (faces outside the tolerance)", transmittance.over, 0);
                report(litCells > 0 && r.errors == 0, "roof: S error bits", r.errors, 0);
            }
        }

        // ---- 5. The far slices behind a ridge: far_shadows on and off.
        if (want("far"))
        {
            // FroxelTests' ridge: 2 km wide, 800 m high, 3 km ahead, a low sun behind it - its shadow fills the air from
            // about 0.3 km to the ridge.
            scene::Scene sc;
            sc.name = "fog ridge";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("ground", { 10000, 0.05f, 10000 }));
            sc.meshes.push_back(boxMesh("ridge", { 1000, 400, 100 }));
            sc.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
            sc.instances.push_back(instanceAt(1, { 0, 400, 3000 }));
            const std::vector<Box> casters = { { { 0, -0.05f, 0 }, { 10000, 0.05f, 10000 } }, { { 0, 400, 3000 }, { 1000, 400, 100 } } };
            sc.sun.direction = normalize(float3{ 0.1f, 0.3f, 1 });
            scene::Camera c;
            c.name = "main";
            c.position = { 0, 2, 0 };
            c.forward = normalize(float3{ 0, 0.05f, 1 });
            sc.cameras.push_back(c);
            setFog(0.0005, 0, 0, 0, white);
            const uint32_t tilePx = shadow::froxelGridFor(tf.quality, W, H).tilePx;
            Options o;
            o.frames = 6;
            const ViewCpu view{ ViewDesc::fromCamera(c, W, H, float4x4{}), d3(c.position) };
            for (uint32_t cy = 0; cy < grid.y; ++cy)
                for (uint32_t cx = grid.x / 4; cx < 3 * grid.x / 4; cx += 5)
                {
                    const D3 ray = view.rayAt((cx + 0.5) * grid.cellPx, (cy + 0.5) * grid.cellPx);
                    if (ray.y > 0.06 && ray.y < 0.24) o.columns.push_back({ cx, cy });  // above the ground with a tile to spare, toward the ridge
                }
            for (int pass = 0; pass < 2; ++pass)
            {
                tf.quality.applyOverride(pass == 0 ? "atmosphere.fog.far_shadows=true" : "atmosphere.fog.far_shadows=false");
                const std::string name = pass == 0 ? "ridge, far_shadows" : "ridge, far_shadows off";
                const Result r = run(sc, o);
                if (!volumeThere(r, name)) continue;
                Tally nearT, litT;
                uint32_t litSlices = 0, darkSlices = 0, darkChanged = 0, mixedSlices = 0, mixedOutside = 0, litOver = 0;
                for (size_t i = 0; i < o.columns.size(); ++i)
                {
                    const uint32_t cx = o.columns[i].first, cy = o.columns[i].second;
                    const double px = (cx + 0.5) * grid.cellPx, py = (cy + 0.5) * grid.cellPx;
                    const Column twin = twinColumn(r.model, cx, cy, &r.sun[i * perColumn]);
                    compareFaces(r.volume, cx, cy, twin, 0, gz, nullptr, nearT, name.c_str());  // the cells are in the sun
                    const double surface = surfaceDepth(r.model, casters, px, py, tilePx);
                    bool allLit = true;
                    for (uint32_t s = 0; s < grid.zFar; ++s)
                    {
                        const double za = farDepth(grid, s), zb = farDepth(grid, s + 1.0);
                        if (!(zb * 1.34 < surface)) break;  // (past the surface the air volume is not integrated: nothing reads it)
                        // the two taps of the air volume's shadowed fraction and the froxel slices each blends: the tap's own
                        // and the one before or after it - within a slice and a half (x 1.33 in depth) either way, one tile to
                        // each side
                        int cls = 1;
                        if (pass == 0)
                        {
                            const double tapA = za * std::pow(zb / za, 0.25), tapB = za * std::pow(zb / za, 0.75);
                            const int a = litClass(r.model, casters, px, py, tilePx, tapA / 1.33, tapA * 1.33), b = litClass(r.model, casters, px, py, tilePx, tapB / 1.33, tapB * 1.33);
                            cls = a == 1 && b == 1 ? 1 : (a == 0 && b == 0 ? 0 : -1);
                        }
                        const std::array<double, 4> before = r.volume.at(cx, cy, gz + s - 1), after = r.volume.at(cx, cy, gz + s);
                        const double full[3] = { twin.L[gz + s][0] - twin.L[gz + s - 1][0], twin.L[gz + s][1] - twin.L[gz + s - 1][1], twin.L[gz + s][2] - twin.L[gz + s - 1][2] };
                        if (cls == 1)
                        {
                            ++litSlices;
                            if (allLit) compareFaces(r.volume, cx, cy, twin, gz + s, gz + s + 1, nullptr, litT, name.c_str());
                            else
                                for (int k = 0; k < 3; ++k)  // after a shadowed slice: the slice's own gain (two fp16 texels)
                                    if (std::abs(after[k] - before[k] - full[k]) > kUlp * (after[k] + before[k]) + 5e-3 * full[k]) ++litOver;
                        }
                        else if (cls == 0)
                        {
                            ++darkSlices;
                            allLit = false;
                            if (!r.volume.sameRadiance(cx, cy, gz + s, gz + s - 1) && darkChanged++ < 5)
                                logf("  %s: cell (%u, %u) far slice %u (%.0f .. %.0f m) is in the shadow: radiance %.6g -> %.6g (the whole source would add %.4g)\n", name.c_str(), cx, cy, s,
                                     za, zb, before[1], after[1], full[1]);
                        }
                        else
                        {
                            ++mixedSlices;
                            allLit = false;
                            for (int k = 0; k < 3; ++k)
                                if (after[k] - before[k] < -kUlp * after[k] || after[k] - before[k] > full[k] * 1.005 + kUlp * (after[k] + before[k])) ++mixedOutside;
                        }
                    }
                }
                logf("%s: %zu columns; far slices in the sun %u, in the ridge's shadow %u, at its boundary %u; the cells' worst rel. L %.3e\n", name.c_str(), o.columns.size(), litSlices,
                     darkSlices, mixedSlices, nearT.worstL);
                report(nearT.over == 0, name + ": the cells before the shadow, vs the twin (faces outside the tolerance)", nearT.over, 0);
                report(litSlices > 50 && litT.over + litOver == 0, name + ": far slices in the sun take the whole source (slices outside the tolerance)", litT.over + litOver, 0);
                if (pass == 0)
                {
                    report(darkSlices > 100 && darkChanged == 0, name + ": far slices in the shadow scatter no sun (slices whose radiance changes)", darkChanged, 0);
                    report(mixedOutside == 0, name + ": slices at the shadow's boundary gain between nothing and the whole source", mixedOutside, 0);
                }
                else report(darkSlices == 0 && mixedSlices == 0, name + ": every far slice before the surface is judged as in the sun", darkSlices + mixedSlices, 0);
                report(r.errors == 0, name + ": S error bits", r.errors, 0);
            }
            tf.quality.applyOverride("atmosphere.fog.far_shadows=true");
        }

        // ---- 6. Local volumes.
        scene::Scene room = open;
        room.cameras[0].position = { 0, 2, 0 };
        room.cameras[0].forward = normalize(float3{ 0, -0.03f, 1 });
        std::vector<std::pair<uint32_t, uint32_t>> lattice;
        for (uint32_t cy = 1; cy < grid.y; cy += 2)
            for (uint32_t cx = 1; cx < grid.x; cx += 2) lattice.push_back({ cx, cy });
        std::vector<FogVolumeDesc> twoVolumes(2);
        {
            FogVolumeDesc& ellipsoid = twoVolumes[0];
            ellipsoid.centre[0] = -6, ellipsoid.centre[1] = 2.5, ellipsoid.centre[2] = 18;
            ellipsoid.halfSize[0] = 4, ellipsoid.halfSize[1] = 2, ellipsoid.halfSize[2] = 5;
            ellipsoid.yaw = 0.6f;
            ellipsoid.shape = 0;
            ellipsoid.density = 0.25f;
            ellipsoid.heightFalloff = 1.5f;
            ellipsoid.edge = 0.3f;
            ellipsoid.albedo[0] = 204 / 255.0f, ellipsoid.albedo[1] = 102 / 255.0f, ellipsoid.albedo[2] = 51 / 255.0f;  // (the record keeps 8 bits per channel)
            FogVolumeDesc& box = twoVolumes[1];
            box.centre[0] = 7, box.centre[1] = 2, box.centre[2] = 25;
            box.halfSize[0] = 3, box.halfSize[1] = 2.5f, box.halfSize[2] = 4;
            box.yaw = -0.4f;
            box.shape = 1;
            box.density = 0.15f;
            box.heightFalloff = 0;
            box.edge = 0.25f;
            box.albedo[0] = 51 / 255.0f, box.albedo[1] = 153 / 255.0f, box.albedo[2] = 1;
        }
        // The cells of a frame's model that hold a local volume's density, and the columns through none.
        auto volumeCells = [&](const Result& r, const std::vector<std::pair<uint32_t, uint32_t>>& columns, uint32_t& inside, std::vector<uint8_t>& clear) {
            Model alone = r.model;
            alone.medium.density = 0;
            inside = 0;
            clear.assign(columns.size(), 1);
            for (size_t i = 0; i < columns.size(); ++i)
                for (uint32_t z = 0; z < gz; ++z)
                {
                    double along = 0;
                    const D3 p = samplePoint(alone, columns[i].first, columns[i].second, z, alone.samples[0], nullptr, &along);
                    if (mediumAt(alone, p, along).sigma > 0)
                    {
                        ++inside;
                        clear[i] = 0;
                    }
                }
        };
        if (want("volumes"))
        {
            Options o;
            o.columns = lattice;
            // with the height fog: the densities add, the albedos mix by their scattering
            setFog(0.004, 0.02, 0, 0, { 0.6f, 0.8f, 1.0f });
            tf.frame.fogVolumes = twoVolumes;
            const Result with = run(room, o);
            if (volumeThere(with, "volumes + height fog"))
            {
                uint32_t inside = 0;
                std::vector<uint8_t> clear;
                volumeCells(with, lattice, inside, clear);
                againstTwin(with, lattice, "ellipsoid + box in the height fog");
                report(inside > 500, "ellipsoid + box: cells of the compared columns that sample a volume", inside, 500);
            }
            // alone: the height fog off (the frame's and the file's), the fog's volume runs for the local volumes
            tf.frame.fog = FogDesc{};
            const Result alone = run(room, o);
            report(alone.word != 0 && alone.hasVolume && alone.fog.on && alone.fog.density == 0, "volumes alone: the fog's volume runs without the height fog (record word)", alone.word, 1);
            if (volumeThere(alone, "volumes alone"))
            {
                uint32_t inside = 0, touched = 0, clearColumns = 0;
                std::vector<uint8_t> clear;
                volumeCells(alone, lattice, inside, clear);
                againstTwin(alone, lattice, "ellipsoid + box alone");
                for (size_t i = 0; i < lattice.size(); ++i)
                {
                    if (!clear[i]) continue;
                    ++clearColumns;
                    for (uint32_t z = 0; z < total; ++z)
                    {
                        const std::array<double, 4> g = alone.volume.at(lattice[i].first, lattice[i].second, z);
                        if (g[0] != 0 || g[1] != 0 || g[2] != 0 || g[3] != 1) ++touched;
                    }
                }
                report(inside > 500 && clearColumns > 100 && touched == 0, "volumes alone: columns through no volume hold no fog (texels that are not (0, 0, 0, 1))", touched, 0);
            }
            // 18 volumes: the first 16 take effect
            std::vector<FogVolumeDesc> many(18);
            for (int i = 0; i < 18; ++i)
            {
                many[i].centre[0] = -17.0 + 2.0 * i, many[i].centre[1] = 1.4, many[i].centre[2] = 20;
                many[i].halfSize[0] = many[i].halfSize[1] = many[i].halfSize[2] = 0.9f;
                many[i].density = 1.0f;
                many[i].edge = 0.3f;
            }
            tf.frame.fogVolumes = many;
            std::vector<std::pair<uint32_t, uint32_t>> through;
            for (int i = 0; i < 18; ++i)
            {
                // the column through the volume's centre (the centre's pixel)
                const ViewDesc view = ViewDesc::fromCamera(room.cameras[0], W, H, float4x4{});
                double clip[4];
                for (int row = 0; row < 4; ++row)
                    clip[row] = view.viewProj.m[row][0] * many[i].centre[0] + view.viewProj.m[row][1] * many[i].centre[1] + view.viewProj.m[row][2] * many[i].centre[2] + view.viewProj.m[row][3];
                const double px = (clip[0] / clip[3] * 0.5 + 0.5) * W, py = (0.5 - clip[1] / clip[3] * 0.5) * H;
                through.push_back({ (uint32_t)(px / grid.cellPx), (uint32_t)(py / grid.cellPx) });
            }
            o.columns = lattice;
            o.columns.insert(o.columns.end(), through.begin(), through.end());
            const Result crowd = run(room, o);
            if (volumeThere(crowd, "18 volumes"))
            {
                againstTwin(crowd, o.columns, "18 volumes: the twin with the first 16");
                double leastOpacity = 1, pastSixteen = 0;
                for (int i = 0; i < 18; ++i)
                {
                    const double T = crowd.volume.at(through[i].first, through[i].second, gz - 1)[3];
                    if (i < 16) leastOpacity = std::min(leastOpacity, 1 - T);
                    else pastSixteen = std::max(pastSixteen, 1 - T);
                }
                report(leastOpacity > 0.2, "18 volumes: each of the first 16 is in its column (least opacity through a centre)", leastOpacity, 0.2);
                report(pastSixteen == 0, "18 volumes: the 17th and 18th leave their columns clear (opacity)", pastSixteen, 0);
            }
            // 1,000 m from the render origin (no rebase): the same room, the camera and the volumes moved together
            {
                const float3 away{ 800, 0, -600 };
                scene::Scene farRoom = room;
                farRoom.cameras[0].position = room.cameras[0].position + away;
                std::vector<FogVolumeDesc> moved = twoVolumes;
                for (FogVolumeDesc& v : moved) v.centre[0] += away.x, v.centre[1] += away.y, v.centre[2] += away.z;
                setFog(0.004, 0.02, 0, 0, { 0.6f, 0.8f, 1.0f });
                tf.frame.fogVolumes = moved;
                Options of;
                of.columns = lattice;
                const Result r = run(farRoom, of);
                if (volumeThere(r, "volumes 1,000 m from the render origin"))
                {
                    uint32_t inside = 0;
                    std::vector<uint8_t> clear;
                    volumeCells(r, lattice, inside, clear);
                    againstTwin(r, lattice, "ellipsoid + box in the height fog, 1,000 m from the render origin");
                    report(inside > 500, "1,000 m from the render origin: cells of the compared columns that sample a volume", inside, 500);
                }
            }
            tf.frame.fogVolumes.clear();
        }

        // ---- 6. The origin offset: the same world from a rebased scene.
        if (want("rebase"))
        {
            // The world shifted by a whole number of 1024 m and of the noise's period (256 lattice points of 4 m): the render
            // space holds the same medium, so the volume must be the same.
            const float3 shift{ 2048, 1024, -1024 };
            tf.quality.applyOverride("atmosphere.fog.noise_drift_mps=0");
            Options o;
            o.columns = lattice;
            o.phase = 5;
            setFog(0.004, 0.03, 1.5, 0, { 0.6f, 0.8f, 1.0f }, 0, 0.3, 4);
            tf.frame.fogVolumes = twoVolumes;
            tf.frame.time = 0;
            const Result at0 = run(room, o);
            scene::Scene moved = room;
            moved.instances[0].transform = float3x4::translation(float3{ -3000, 1, -3000 } + shift);
            setFog(0.004, 0.03, 1.5 + shift.y, 0, { 0.6f, 0.8f, 1.0f }, 0, 0.3, 4);
            for (FogVolumeDesc& v : tf.frame.fogVolumes) v.centre[0] += shift.x, v.centre[1] += shift.y, v.centre[2] += shift.z;
            o.rebase = shift;
            tf.frame.time = 0;
            const Result at1 = run(moved, o);
            const float3 offset = tf.gpuScene.originOffset();
            report(offset.x == shift.x && offset.y == shift.y && offset.z == shift.z, "rebase: the scene's origin offset is the shift (x)", offset.x, shift.x);
            if (volumeThere(at0, "rebase, before") && volumeThere(at1, "rebase, after"))
            {
                uint32_t inside = 0;
                std::vector<uint8_t> clear;
                volumeCells(at1, lattice, inside, clear);
                againstTwin(at0, lattice, "world at the origin: height fog 0.03 at 1.5 m, noise over 4 m, two volumes");
                againstTwin(at1, lattice, "the same world 2048, 1024, -1024 m away, rebased");
                uint64_t differing = 0;
                double worst = 0;
                for (uint32_t z = 0; z < total; ++z)
                    for (uint32_t cy = 0; cy < grid.y; ++cy)
                        for (uint32_t cx = 0; cx < grid.x; ++cx)
                        {
                            const std::array<double, 4> a = at0.volume.at(cx, cy, z), b = at1.volume.at(cx, cy, z);
                            differing += std::memcmp(at0.volume.texel(cx, cy, z), at1.volume.texel(cx, cy, z), 8) != 0 ? 1 : 0;
                            for (int k = 0; k < 4; ++k) worst = std::max(worst, std::abs(a[k] - b[k]) / std::max(std::abs(a[k]), 1e-3));
                        }
                logf("rebase: %llu of %u texels differ in their bits; largest relative difference %.3e; %u compared cells sample a volume\n", (unsigned long long)differing,
                     grid.x * grid.y * total, worst, inside);
                report(inside > 500 && worst <= 2 * kUlp, "rebase: the fog's height, the volumes and the noise stay in place (largest rel. difference of a texel)", worst, 2 * kUlp);
            }
            tf.frame.fogVolumes.clear();
            tf.quality.applyOverride(format("atmosphere.fog.noise_drift_mps=%.9g", (double)defaultFog.noiseDrift));
            // (GpuScene keeps its origin offset over an upload: back to the origin for what follows)
            tf.gpuScene.rebase(float3{ -shift.x, -shift.y, -shift.z });
            tf.gpuScene.flushUpdates(tf.frame.frameIndex, 2, tf.shaders);
            const float3 back = tf.gpuScene.originOffset();
            report(back.x == 0 && back.y == 0 && back.z == 0, "rebase: the origin is back for the next scenes (offset x)", back.x, 0);
        }

        // ---- 7. A planar reflection view's volume: a lake at y = 0 seen from 3 m above it.
        if (want("planar"))
        {
            scene::Scene lake = open;
            lake.cameras[0].position = { 0, 3, 0 };
            lake.cameras[0].forward = normalize(float3{ 0, -0.15f, 1 });
            setFog(0.01, 0.05, 2, 0.3, { 0.9f, 0.95f, 1.0f });
            Options o;
            o.columns = spread;
            o.mirror = true;
            o.mirrorPlane = { 0, 1, 0, 0 };
            const Result r = run(lake, o);
            report(r.hasPlanar, "planar: the reflection view has its own fog volume", r.hasPlanar, 1);
            if (r.hasPlanar && volumeThere(r, "planar, the main view"))
            {
                againstTwin(r, spread, "planar: the main view beside it");
                Tally nearT, farT;
                uint32_t before = 0, fogBefore = 0, crossing = 0, fogNever = 0;
                const Model& m = r.planarModel;
                for (size_t i = 0; i < spread.size(); ++i)
                {
                    const uint32_t cx = spread[i].first, cy = spread[i].second;
                    const Column c = twinColumn(m, cx, cy, &r.planarSun[i * perColumn]);
                    compareFaces(r.planar, cx, cy, c, 0, gz, nullptr, nearT, "planar view");
                    compareFaces(r.planar, cx, cy, c, gz, total, nullptr, farT, "planar view");
                    // the mirror on the column's centre ray (the virtual camera is under the lake)
                    const D3 ray = m.view.rayAt((cx + 0.5) * grid.cellPx, (cy + 0.5) * grid.cellPx);
                    const double cross = ray.y > 0 ? -m.view.cam.y / ray.y : INFINITY;  // view depth where the ray reaches y = 0
                    if (cross < grid.farM) ++crossing;
                    for (uint32_t z = 0; z < total; ++z)
                    {
                        // every sample of the slices up to z is under the lake: the cell centres' heights
                        const double depth = z < gz ? depthOfSlice(grid, z + 1.0) : farDepth(grid, z - gz + 1.0);
                        if (!(depth < cross)) break;
                        const std::array<double, 4> g = r.planar.at(cx, cy, z);
                        ++before;
                        if (g[0] != 0 || g[1] != 0 || g[2] != 0 || g[3] != 1) (std::isfinite(cross) ? fogBefore : fogNever)++;
                    }
                }
                logf("planar: %u cell faces, worst rel. L %.3e T %.3e; %u far faces, L %.3e T %.3e; %u columns cross the mirror inside the cells; %u faces before it\n", nearT.faces,
                     nearT.worstL, nearT.worstT, farT.faces, farT.worstL, farT.worstT, crossing, before);
                report(nearT.over == 0 && farT.over == 0, "planar: the view's volume vs the twin from the mirror on (faces outside the tolerance)", nearT.over + farT.over, 0);
                report(before > 500 && fogBefore + fogNever == 0, "planar: no fog before the mirror plane (faces that are not (0, 0, 0, 1))", fogBefore + fogNever, 0);
                report(crossing > 5, "planar: columns that reach the mirror inside the cells", crossing, 5);
            }
        }

        tf.frame.fog = FogDesc{};
        report(shadow::stats(tf.trackState).errorBitsSeen == 0, "S error bits (INTERFACES 3.6: a shader loop at its hard cap)", shadow::stats(tf.trackState).errorBitsSeen, 0);
        if (debugLayer) logf("D3D12 debug layer: enabled (errors abort the run)\n");
        logf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
