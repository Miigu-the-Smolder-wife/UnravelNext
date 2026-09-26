// Coverage composite: shared definitions of its kernels (design COVERAGE_REDESIGN 4.5; INTERFACES 7.1 v1.39). Owner: M.
// Every dispatch's work is bounded by the kernel's structure (a group handles at most COV_BLOCK records, one pixel of at
// most COV_LIGHT fragments, one 1,024-pair run or one round of COV_ROUND fragments of a heavy pixel), so the frame's cost
// follows the data and spreads over the GPU whatever the distribution (two TDRs on forest scenes with a per-tile walk,
// 2026-09-26). Loops over data-dependent counts are capped by engine constants (pool capacity) and set
// COV_M_ERROR_ITERATION_LIMIT (INTERFACES 3.6) if a cap is ever reached.
// V's records come pixel-major per listed tile (INTERFACES 7.1 v1.41): a pixel's fragments are one range, in no order.
// Stages (ShadingSystem.cpp, all indirect, sizes made on the GPU):
//   CoverageBegin         reset the state words; the heavy passes' and each round's arguments
//   CoverageComposite     per listed tile: light pixels (<= COV_LIGHT fragments) sorted in registers and composited;
//                         heavy pixels recorded
//   CoverageHeavySort     per (heavy pixel, run of COV_BLOCK records): (depth key, element) pairs sorted in groupshared
//                         into M's pair buffer (indexed like V's records)
//   CoverageHeavyRound    one wave per heavy pixel, a fixed number of dispatches: the next COV_ROUND fragments merged
//                         from the runs, shaded in parallel, weighted by the mask-union prefix, accumulated
//   CoverageHeavyFinish   per heavy pixel: the band A remainder, the output; unfinished = error bit
// Kernels that shade fragments share the constants P[1] = { visible clusters, M texture table, band A depth, colour UAV },
// P[3] = { froxel lights or UNX_NONE, LTC table, experiment mask, S fragment visibility or UNX_NONE }, P[4] = { atmosphere
// transmittance, multi-scatter, air volume, R's screen probes }, P[5].x = R's screen probe maps (UNX_NONE = absent).
#ifndef UNX_M_COVERAGE_SHADE_HLSLI
#define UNX_M_COVERAGE_SHADE_HLSLI
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Visibility/Coverage.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#define GI_PROBE_TILE_CACHE
#include "Passes/GI/ScreenProbes.hlsli"

#define COV_LIGHT 16u     // fragments of a light pixel (sorted in registers)
#define COV_ROUND 32u     // fragments a heavy pixel's round takes
#define COV_M_ERROR_ITERATION_LIMIT 0x400u
#define COV_M_ERROR_ROUNDS 0x800u  // a heavy pixel still open after the frame's rounds (more next frame)

// State words (raw buffer 'm.coverage.state').
#define COVS_HEAVY 2u       // heavy pixels
#define COVS_ERRORS 3u      // error bits (copied to M's statistics)
#define COVS_RUNS 4u        // runs of all heavy pixels (cursor slots)
#define COVS_MAX_RUNS 5u    // the most runs of one heavy pixel
#define COVS_OPEN 6u        // 6..7 open heavy pixels appended to active list 0 / 1 by a round
#define COVS_WORDS 8u
// Dispatch arguments (raw buffer 'm.coverage.args', written by CoverageBegin, read as arguments).
#define COVA_SORT 4u        // 4..6 (heavy pixels x, y, most runs of one)
#define COVA_ROUND 8u       // 8..10 the next heavy round (open pixels)
#define COVA_FINISH 12u     // 12..14 heavy pixels / 64
#define COVA_WORDS 16u
#define COV_ROUNDS 8u       // heavy rounds per frame: up to 256 fragments composited per pixel (design 4.5: more is a defect)
// Heavy pixel record (raw buffer 'm.coverage.heavy', COVH_WORDS words each).
#define COVH_PIXEL 0u       // x | y << 16
#define COVH_SEGMENT 1u     // first pair
#define COVH_COUNT 2u       // fragments
#define COVH_RUNS 3u        // runs of COV_BLOCK pairs
#define COVH_CURSORS 4u     // first cursor slot (one per run)
#define COVH_COVERED 5u     // mask union so far
#define COVH_USED 6u        // sum of weights so far (float bits)
#define COVH_DONE 7u        // 1: complete (union full, weights at 1, fragments exhausted or behind band A)
#define COVH_SUM 8u         // 8..10 exposed radiance so far (float bits)
#define COVH_LAST 11u       // 11 the last fragment's depth key, 12 its vis id (clip duplicate check)
#define COVH_WORDS 16u

// Band A radiance under a pixel's fragments: what the shading kernels kept for coverage tiles, or the E composite's
// resolved sum on edge pixels. P[2] = { band A radiance SRV, resolved SRV or UNX_NONE, edge tile mask SRV or UNX_NONE, .. }.
float3 covBandA(uint2 pixel)
{
    Texture2D<float4> bandRadiance = ResourceDescriptorHeap[P[2].x];
    if (P[2].y != UNX_NONE && P[2].z != UNX_NONE)
    {
        Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[2].z];
        const uint2 edgeMask = edgeTiles[pixel / COV_TILE_PX];
        const uint bit = (pixel.x % COV_TILE_PX) + COV_TILE_PX * (pixel.y % COV_TILE_PX);
        if ((((bit < 32 ? edgeMask.x : edgeMask.y) >> (bit & 31)) & 1u) != 0)
        {
            Texture2D<float4> resolved = ResourceDescriptorHeap[P[2].y];
            return resolved[pixel].rgb;
        }
    }
    return bandRadiance[pixel].rgb;
}

// R's screen probes of the group's tile into the tile cache (lanes 0..24; the caller syncs the group).
uint4 covProbeFetch(uint2 tileCoord, uint lane)
{
    if (P[4].w == UNX_NONE || (P[3].z & 6) == 6) return 0;
    ProbeSrvs probes;
    probes.probes = P[4].w;
    probes.occlusion = P[4].w;
    probes.pad0 = P[5].x;
    probes.pad1 = 0;
    return giProbeTileFetch(probes, tileCoord, lane, giProbeCountOfView());
}

// Order key of a fragment: nearer first (reversed-Z depth, see-through bit dropped), then pool element. a before b.
bool covBefore(uint2 a, uint2 b) { return a.x > b.x || (a.x == b.x && a.y < b.y); }
uint2 covKey(StructuredBuffer<uint4> records, uint element) { return uint2(records[element].y & ~COV_DEPTH_SEE_THROUGH, element); }
static const uint2 COV_KEY_AFTER_ALL = uint2(0, 0xFFFFFFFFu);  // after every key (a record's element is below it)


// Pixel coordinates of a camera-relative point (x right, y down), w = view depth (EdgeComposite.hlsl edgeProject).
float3 covProject(float3 offset)
{
    const float3 v = float3(dot(g_view[0].xyz, offset), dot(g_view[1].xyz, offset), dot(g_view[2].xyz, offset));
    const float4 clip = mul(g_proj, float4(v, 1));
    return float3((clip.x / clip.w * 0.5 + 0.5) * g_viewWidth, (0.5 - clip.y / clip.w * 0.5) * g_viewHeight, clip.w);
}


// Exposed linear radiance of one fragment (the band A kernel's lighting for a surface point: ShadeOpaque.hlsl).
float3 covShadeFragment(uint visId, uint2 pixel, uint experiment)
{
    // The triangle and the covered region's centroid in this pixel, where V evaluated the fragment's depth.
    const MTriangleIdentity id = mTriangleIdentity(visId, P[1].x);
    const MVertex v0 = mTriangleVertex(visId, P[1].x, 0), v1 = mTriangleVertex(visId, P[1].x, 1), v2 = mTriangleVertex(visId, P[1].x, 2);
    const float3 a = covProject(v0.world - g_cameraPosition), b = covProject(v1.world - g_cameraPosition), c = covProject(v2.world - g_cameraPosition);
    float2 centre = float2(pixel) + 0.5;
    if (min(a.z, min(b.z, c.z)) > 0) coverageTriangleAreaCentroid(a.xy, b.xy, c.xy, float2(pixel), centre);
    const MSurface sf = mSurfaceFromVertices(id, v0, v1, v2, centre);

    // Material at the footprint (Resolve.hlsl).
    const GpuMaterial m = loadMaterial(sf.material);
    const MTextureSet ts = mLoadTextureSet(P[1].y, sf.material);
    float3 baseColor = m.baseColor;
    if (ts.baseColor != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
        baseColor *= mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, sf.uv, sf.duvdx, sf.duvdy).rgb;
    }
    float roughness = m.roughness, metallic = m.metallic;
    if (ts.roughMetal != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
        const float2 rm = mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, sf.uv, sf.duvdx, sf.duvdy).xy;
        roughness *= rm.x;
        metallic *= rm.y;
    }
    float variance = (dot(sf.dndx, sf.dndx) + dot(sf.dndy, sf.dndy)) / 12.0;
    float3 n;
    if (ts.moments != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
        const MSlopeMoments mm = mNormalMoments(t, sf.uv, sf.duvdx, sf.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
        const float3 B = sf.tangentSign * cross(sf.normal, sf.tangent);
        n = normalize(sf.tangent * mm.mean.x + B * mm.mean.y + sf.normal);
        variance += mm.variance;
    }
    else n = normalize(sf.normal);
    if (!sf.front && (m.classFlags & MATERIAL_TWO_SIDED) != 0) n = -n;
    const float alphaIn = max(roughness * roughness, 1e-4);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = baseColor;
    s.roughness = min(sqrt(sqrt(alphaIn * alphaIn + variance)), 1.0);
    s.metallic = metallic;
    s.specular = m.specular;
    s.transmission = m.transmission;
    float3 radiance = m.emissive;
    if (ts.emissive != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.emissive];
        radiance = m.emissive * mSampleGrad(t, (ts.flags & M_TEX_EMISSIVE) != 0, sf.uv, sf.duvdx, sf.duvdy).rgb;
    }

    float3 D, Dx, Dy;
    mPixelRay(centre, D, Dx, Dy);
    const float3 offset = sf.offset;
    const float3 worldPos = g_cameraPosition + offset;
    const float linearZ = dot(offset, -g_view[2].xyz);  // view depth (D has unit depth along the view axis)
    const float3 v = sf.view;
    const float NoV = dot(n, v);
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);
    const float3 f0 = modelF0(s);
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;

    // ---- sun (with S's air at the fragment's depth: in-scatter and transmittance in front of it, the sun's illuminance)
    AtmosphereSrvs atm;
    atm.transmittance = P[4].x;
    atm.multiScatter = P[4].y;
    atm.skyView = UNX_NONE;
    atm.aerial = P[4].z;
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (atm.transmittance != UNX_NONE)
    {
        if (atm.aerial != UNX_NONE && (experiment & 8) == 0)
            atmosphereAirView(atm, centre / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    const float3 l0 = normalize(g_sunDirection);
    const float NoL = dot(n, l0);
    const float sunVisibility = 1;  // S's fragment visibility (4.3) once published: see the header
    if (sunVisibility > 0 && (experiment & 16) == 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));
        const float above = shCapCosine(NoL), below = shCapCosine(-NoL);
        float3 sun = 0;
        if (NoV > 0)
        {
            if (above > 0)
            {
                const float e = modelDirectionalAlbedo(NoV, s.roughness);
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap + shSunSpecular(f0, s.roughness, alpha, compensation, n, v, NoV, l0, E, shPixelAngle(D, Dx));
            }
            if (foliage) sun += back * below * cap;
        }
        else if (foliage) sun = back * above * cap;
        radiance += sun * sunVisibility;
    }

    // ---- local lights of the froxel list at the fragment's depth
    FroxelSrvs froxels;
    froxels.lights = P[3].x;
    froxels.lightIndices = P[3].x;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    if (froxels.lights != UNX_NONE && (experiment & 32) == 0)
    {
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
        const float3 compensation = 1 + f0 * (1 / e - 1);
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
#if AREA
        const float3x3 frame = shShadingFrame(n, v, NoV);
        const float3x3 frameBack = float3x3(frame[0], -frame[1], -frame[2]);
        const float3x3 specular = mul(shLtcInverse(P[3].y, max(NoV, 1e-4), s.roughness), frame);
        const float3 specularAlbedo = shSpecularAlbedo(f0, max(NoV, 1e-4), s.roughness);
#endif
        for (uint i = 0; i < range.y; ++i)
        {
            const GpuLight light = loadLight(froxelLight(froxels, range.x + i));
            const float visibility = 1;  // S's fragment slots (4.3) once published
            if (lightType(light) > LIGHT_SPOT)
            {
#if AREA
                const float3 p = (light.position - g_cameraPosition) - offset;
                const float window = shAreaWindow(light, p);
                if (window <= 0) continue;
                const float3 Lw = light.color * (light.intensity * window * visibility);
                const uint first = NoV > 0 ? 0 : 2, last = foliage ? 3 : 2;
                [loop] for (uint j = first; j < last; ++j)
                {
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (NoV > 0 ? frameBack : frame));
                    const float I = shAreaIntegral(light, p, T, j != 1);
                    radiance += Lw * (j == 0 ? front * (SH_PI * I) : (j == 1 ? specularAlbedo * I : back * (SH_PI * I)));
                }
#endif
                continue;
            }
            float3 l;
            const float3 El = shPunctualIlluminance(light, (light.position - g_cameraPosition) - offset, l);
            const float cosL = dot(n, l);
            float3 f = 0;
            if (NoV > 0 && cosL > 0) f = front + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
            radiance += f * El * (abs(cosL) * visibility);
        }
    }

    // ---- indirect: R's screen probes from the tile cache (irradiance; the K path for the specular lobe)
    if (P[4].w != UNX_NONE && (experiment & 6) != 6)
    {
        ProbeSrvs probes;
        probes.probes = P[4].w;
        probes.occlusion = P[4].w;
        probes.pad0 = P[5].x;
        probes.pad1 = 0;
        const float3 nv = NoV > 0 ? n : -n;
        const float3 r = reflect(-v, n);
        const bool wantRadiance = NoV > 0 && (experiment & 4) == 0;
        const ScreenProbeLighting g = screenProbeGatherTile(probes, pixel / M_TILE, pixel, worldPos, nv, linearZ, foliage && (experiment & 2) == 0, wantRadiance, r,
                                                            reflectionLobeHalfAngle(s.roughness, NoV));
        if ((experiment & 2) == 0)
        {
            if (NoV > 0) radiance += front * (g.irradiance * g.occlusion);
            radiance += back * (g.irradianceBack * g.occlusion);
        }
        if (wantRadiance) radiance += g.radiance * shSpecularAlbedo(f0, NoV, s.roughness);
    }
    return (radiance * airTransmittance + airInscatter) * g_exposure;
}

#endif
