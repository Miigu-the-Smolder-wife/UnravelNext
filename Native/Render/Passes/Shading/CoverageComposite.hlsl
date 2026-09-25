// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1 AREA=0,1
// Coverage composite (design COVERAGE_REDESIGN 4.5; INTERFACES 7.1 v1.38): one group per 8 x 8 tile of V's coverage
// tile list (every tile with fragments, heavy ones included). V's records (16 B: vis id, centroid depth, 32-subsample
// mask, normal | area | pixel) are in append order, so the group first sorts them by pixel:
//   1. counts per pixel (groupshared), a base in M's scratch by one atomic per tile, the records' pool elements scattered
//      pixel-major into it (no groupshared record array, so tiles of any size take the same path and occupancy stays
//      that of the shading work);
//   2. each pixel's lane walks its fragments front to back by selection: the next one strictly after the last in the order
//      (depth descending: reversed Z, nearer first; vis id ascending), which also drops the hardware's clip duplicates (same
//      vis id and depth). It stops once the pixel's subsamples are covered or the band A surface is reached, so a pixel pays
//      (fragments x visible ones) key reads, not a sort.
// Composite (the E composite's rule): a fragment covers area x the share of its mask no nearer fragment covered (<= 1/32
// of an overlap), capped by what the nearer ones left; only fragments with a visible share are shaded; the band A surface
// (behind every fragment before it) takes the remainder with its exposed linear radiance, which the shading kernels kept
// for the pixels of coverage tiles (the E composite's resolved sum on edge pixels). The sum is tone mapped once.
// Fragment shading: the triangle the vis id names, through the covered region's centroid (where V took the depth:
// coverageTriangleAreaCentroid), with the material resolve's surface (MaterialSurface.hlsli: uv and its footprint, normal
// map moments, band-limited roughness), then the band A kernel's lighting: sun over the disk, local lights of the froxel
// list at the fragment's depth (area lights under AREA=1), R's screen probes from the tile cache (irradiance and the K
// path lobe), S's air at the fragment's depth. Fragments take no R reflection (G/M results are band A pixels').
// Shadows: S's fragment visibility (COVERAGE_REDESIGN 4.3) is P[3].w; until S publishes it the shading system allows the
// layer only without S's visibility (unshadowed, as the band A kernel without S), and fails otherwise.
// See-through records (glass, water) are composited as opaque: those classes are shaded with the opaque model until
// theirs are defined (INTERFACES 8.1), as in band A.
// P[0] = { coverage tiles (raw), chunk table (raw), chunk records (StructuredBuffer<uint4>), tile list (raw) }
// P[1] = { visible clusters SRV, M texture table SRV, band A depth SRV, colour UAV }
// P[2] = { band A exposed radiance SRV (RGBA16F), E composite's resolved radiance SRV or UNX_NONE, edge tile mask SRV
//        (R32G32_UINT) or UNX_NONE, M scratch UAV (raw: word 0 allocation counter, then pool elements) }
// P[3] = { froxel lights (raw) or UNX_NONE, LTC table SRV, experiment mask (shading.experiment_disable), S fragment
//        visibility or UNX_NONE }
// P[4] = { atmosphere transmittance, multi-scatter, this view's air volume, R's screen probes } (UNX_NONE = absent)
// P[5] = { R's screen probe maps (K path) or UNX_NONE, 0, 0, 0 }
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

groupshared uint gs_count[COV_TILE_PIXELS];   // fragments per pixel
groupshared uint gs_cursor[COV_TILE_PIXELS];  // scatter cursor, then the pixel's first slot
groupshared uint gs_base;                     // the tile's first scratch word

// Pixel coordinates of a camera-relative point (x right, y down), w = view depth (EdgeComposite.hlsl edgeProject).
float3 covProject(float3 offset)
{
    const float3 v = float3(dot(g_view[0].xyz, offset), dot(g_view[1].xyz, offset), dot(g_view[2].xyz, offset));
    const float4 clip = mul(g_proj, float4(v, 1));
    return float3((clip.x / clip.w * 0.5 + 0.5) * g_viewWidth, (0.5 - clip.y / clip.w * 0.5) * g_viewHeight, clip.w);
}

// Order key of a record: nearer first (reversed-Z depth, sign bit dropped), then vis id. True when a is before b.
bool covBefore(uint2 a, uint2 b) { return a.x > b.x || (a.x == b.x && a.y < b.y); }

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

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[P[2].w];
    const uint listed = gid.x + gid.y * 65535;
    if (listed >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform: the whole group leaves
    const uint tile = list.Load(4 * (COV_LIST_TILES + listed)), tilesX = list.Load(4 * COV_LIST_TILES_X), slots = list.Load(4 * COV_LIST_TABLE_SLOTS);
    const uint2 tileCoord = uint2(tile % tilesX, tile / tilesX);
    const uint n = tiles.Load(4 * (tile * COV_TILE_WORDS + COV_TILE_COUNT)), ext = tiles.Load(4 * (tile * COV_TILE_WORDS + COV_TILE_EXT));
    const uint experiment = P[3].z;

    // R's screen probes around the tile (the band A kernel's tile cache) and the per-pixel counts.
    uint4 probeRecord = 0;
    const bool probeTile = P[4].w != UNX_NONE && (experiment & 6) != 6;
    if (probeTile)
    {
        ProbeSrvs probes;
        probes.probes = P[4].w;
        probes.occlusion = P[4].w;
        probes.pad0 = P[5].x;
        probes.pad1 = 0;
        probeRecord = giProbeTileFetch(probes, tileCoord, gi, giProbeCountOfView());
    }
    gs_count[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint i = gi; i < n; i += COV_TILE_PIXELS)
    {
        const uint chunk = coverageChunkOf(table, records, slots, tile, ext, i / COV_CHUNK_RECORDS);
        if (chunk == 0) continue;  // pool overflow (V's statistics fail the gate)
        InterlockedAdd(gs_count[coverageFragmentPixel(coverageLoadRecord(records, chunk, i))], 1);
    }
    if (probeTile) giProbeTileStore(gi, probeRecord);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        uint base = 0;
        scratch.InterlockedAdd(0, n, base);
        gs_base = 1 + base;
        uint start = 0;
        for (uint p = 0; p < COV_TILE_PIXELS; ++p)
        {
            gs_cursor[p] = start;
            start += gs_count[p];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint base = gs_base;
    const uint first = gs_cursor[gi];  // this pixel's first slot, read before the scatter moves the cursors
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < n; j += COV_TILE_PIXELS)
    {
        const uint chunk = coverageChunkOf(table, records, slots, tile, ext, j / COV_CHUNK_RECORDS);
        if (chunk == 0) continue;
        const uint element = (chunk - 1) * COV_CHUNK_RECORDS + j % COV_CHUNK_RECORDS;
        uint slot;
        InterlockedAdd(gs_cursor[coverageFragmentPixel(coverageUnpackRecord(records[element]))], 1, slot);
        scratch.Store(4 * (base + slot), element);
    }
    DeviceMemoryBarrierWithGroupSync();

    const uint2 pixel = tileCoord * COV_TILE_PX + uint2(gi % COV_TILE_PX, gi / COV_TILE_PX);
    const uint count = gs_count[gi];
    if (count == 0 || any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].z];
    const float bandA = bandDepth[pixel];

    // Front to back by selection; the band A surface ends the walk.
    float3 sum = 0;
    float used = 0;
    uint covered = 0;
    uint2 last = uint2(0xFFFFFFFFu, 0);  // before every key
    bool started = false;
    for (;;)
    {
        uint2 best = 0;
        uint bestElement = 0;
        bool found = false;
        for (uint k = 0; k < count; ++k)
        {
            const uint element = scratch.Load(4 * (base + first + k));
            const uint2 key = uint2(records[element].y & ~COV_DEPTH_SEE_THROUGH, records[element].x);
            if ((!started || covBefore(last, key)) && (!found || covBefore(key, best)))
            {
                best = key;
                bestElement = element;
                found = true;
            }
        }
        if (!found || asfloat(best.x) < bandA) break;  // no more fragments, or the rest lie behind the band A surface
        started = true;
        last = best;
        const CoverageFragment f = coverageUnpackRecord(records[bestElement]);
        const uint bits = countbits(f.mask);
        const float seen = bits > 0 ? countbits(f.mask & ~covered) / (float)bits : 1 - countbits(covered) / 32.0;
        const float w = min(coverageFragmentArea(f) * seen, max(1 - used, 0.0));
        if (w > 0) sum += w * covShadeFragment(f.visId, pixel, experiment);
        used += w;
        covered |= f.mask;
        if (covered == COV_MASK_FULL) break;  // every later fragment is hidden
    }

    // The band A surface takes what the fragments left (its exposed radiance: the E composite's sum on edge pixels).
    const float wA = max(1 - used, 0.0);
    if (wA > 0)
    {
        Texture2D<float4> bandRadiance = ResourceDescriptorHeap[P[2].x];
        float3 LA = bandRadiance[pixel].rgb;
        if (P[2].y != UNX_NONE && P[2].z != UNX_NONE)
        {
            Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[2].z];
            const uint2 edgeMask = edgeTiles[tileCoord];
            if ((((gi < 32 ? edgeMask.x : edgeMask.y) >> (gi & 31)) & 1u) != 0)
            {
                Texture2D<float4> resolved = ResourceDescriptorHeap[P[2].y];
                LA = resolved[pixel].rgb;
            }
        }
        sum += wA * LA;
    }
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].w];
    color[pixel] = shEncodeExposed(sum);
}
