// unx-kernel: cs_6_6 main
// Coverage records' FAR field (RENDERER_REDESIGN_V2 14.1c, L2c; owner A): one group of 64 threads per listed coverage
// tile (V's tile list), writing the tile's field (CoverageTileLights.hlsli): the records' depth span cut into K <= 16
// intervals of equal ratio (at least COV_TL_RATIO: 2.6 % of depth, the 1e-3 interpolation rule for a light at the
// interval's distance; a wider interval moves closer lights to NEAR through condition 1), per interval the froxel list of
// its representative point classified (conditions 1-2 and shadow; no horizon condition: the bins carry it), the FAR
// lights' vector irradiance at that point summed per direction bin (thread b < 26 sums bin b in entry order:
// deterministic, no atomics), the NEAR mask and each entry's bin id. Tiles past the field's capacity (P[1].x) are not
// written: their records keep the per-record loop (CoverageShade.hlsli), exact.
// P[0] = { V's records (StructuredBuffer<uint4>), V's tile list (raw), froxel lights SRV, field UAV (raw) }
// P[1] = { field capacity (tiles), the lighting channels every instance is in (GpuScene::lightingChannelsShared: a
//          light in none of them is NEAR - the fragment's loop tests its instance), 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Common/LightNearFar.hlsli"
#include "Passes/Lights/CoverageTileLights.hlsli"

groupshared uint gs_zMinBits, gs_zMaxBits;
groupshared uint gs_K;
groupshared float gs_zMin, gs_lnRatio;
groupshared uint2 gs_list;        // the current interval's (first, count)
groupshared uint gs_slice;
groupshared float3 gs_point;      // camera-relative representative point
groupshared uint gs_near[64];     // entry verdict: 1 NEAR
groupshared uint gs_bin[64];      // entry bin (FAR)
groupshared uint gs_mask[2];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint listed = gid.x;
    if (listed >= list.Load(4 * COV_LIST_COUNT) || listed >= P[1].x) return;  // (uniform)
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer field = ResourceDescriptorHeap[P[0].w];
    const uint tileBase = listed * COV_TL_TILE_BYTES;
    const uint4 info = coverageTileInfo(list, listed);  // tile, records, record base, block base
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    const uint2 tileCoord = uint2(info.x % tilesX, info.x / tilesX);

    // 1. the records' depth span
    if (lane == 0) { gs_zMinBits = 0x7F800000u; gs_zMaxBits = 0; }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = lane; j < info.y; j += 64)
    {
        const float device = asfloat(records[info.z + j].y & ~COV_DEPTH_SEE_THROUGH);
        if (device <= 0) continue;
        const uint zBits = asuint(linearDepth(device));  // positive floats order as uints
        InterlockedMin(gs_zMinBits, zBits);
        InterlockedMax(gs_zMaxBits, zBits);
    }
    GroupMemoryBarrierWithGroupSync();
    FroxelSrvs f;
    f.lights = P[0].z;
    f.lightIndices = P[0].z;
    f.scattering = UNX_NONE;
    f.pad = 0;
    const FroxelGrid g = froxelGrid(f.lights);
    if (lane == 0)
    {
        const float zMin = asfloat(gs_zMinBits), zMax = asfloat(gs_zMaxBits);
        const bool any = gs_zMaxBits != 0 && zMax >= zMin;
        float lnRatio = log(COV_TL_RATIO);
        uint K = 1;
        if (any && zMax > zMin)
        {
            K = clamp((uint)ceil(log(zMax / zMin) / lnRatio), 1u, COV_TL_INTERVALS);
            lnRatio = max(log(zMax / zMin) / K, log(COV_TL_RATIO)) * 1.0000001;  // the actual ratio (wider when K hit 16)
        }
        gs_K = any ? K : 0;
        gs_zMin = zMin;
        gs_lnRatio = lnRatio;
        field.Store4(tileBase, uint4(any ? 1u : 0u, gs_K, asuint(zMin), asuint(lnRatio)));
        field.Store4(tileBase + COV_TL_SLICES_OFFSET, uint4(0, 0, 0, 0));
    }
    GroupMemoryBarrierWithGroupSync();
    const uint K = gs_K;
    float3 D, Dx, Dy;
    mPixelRay(float2(tileCoord * COV_TILE_PX) + 4.0, D, Dx, Dy);  // the tile-centre ray, unit view depth
    const uint indexBase = froxelIndexBase(f);
    const float pixelWidthPerDepth = 2 * g_tanHalfFovY / g_viewHeight;

    // 2. intervals (sequential in the group; every store below is the group's own tile)
    for (uint k = 0; k < K; ++k)
    {
        if (lane == 0)
        {
            const float z0 = gs_zMin * exp(gs_lnRatio * k), z1 = gs_zMin * exp(gs_lnRatio * (k + 1)), zm = sqrt(z0 * z1);
            gs_slice = froxelSlice(g, zm);
            gs_point = D * zm;
            const uint2 range = froxelLightRange(f, tileCoord * COV_TILE_PX + 4, zm);
            gs_list = range.y > 64 ? uint2(range.x, 0xFFFFFFFFu) : range;  // over 64 entries: no field for this interval
            gs_mask[0] = 0;
            gs_mask[1] = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        const uint2 range = gs_list;
        const bool fieldable = range.y != 0xFFFFFFFFu;
        const float z0 = gs_zMin * exp(gs_lnRatio * k), z1 = gs_zMin * exp(gs_lnRatio * (k + 1)), zm = sqrt(z0 * z1);
        NfRegion region;
        region.centre = g_cameraPosition + gs_point;
        const float lateral = 4 * pixelWidthPerDepth * zm;  // half the tile's width at the representative depth
        region.halfExtent = sqrt(0.25 * (z1 - z0) * (z1 - z0) + 2 * lateral * lateral);  // depth half span, the tile's half diagonal
        region.normalAxis = float3(0, 1, 0);
        region.normalSin = 0;
        // 2a. classification and bins: one entry per thread
        gs_near[lane] = 1;
        gs_bin[lane] = 0;
        if (fieldable && lane < range.y)
        {
            const GpuLight light = loadLight(froxelLightAt(f, indexBase, range.x + lane));
            float d;
            const bool near = lightCastsShadow(light) || nfIsNearGeometricNoHorizon(light, region, d) || !nfLightsEveryInstance(light, P[1].y);
            gs_near[lane] = near ? 1u : 0u;
            if (near) InterlockedOr(gs_mask[lane >> 5], 1u << (lane & 31));
            else gs_bin[lane] = covTlBinOf(normalize(light.position - region.centre));
        }
        GroupMemoryBarrierWithGroupSync();
        // 2b. bin sums: thread b sums bin b over the entries in order (each FAR light once, by exactly one thread)
        if (lane < COV_TL_BINS)
        {
            float3 e = 0;
            if (fieldable)
                for (uint i = 0; i < range.y; ++i)
                    if (gs_near[i] == 0 && gs_bin[i] == lane)
                        e += nfVectorIrradiance(loadLight(froxelLightAt(f, indexBase, range.x + i)), region.centre);
            field.Store3(tileBase + COV_TL_E_OFFSET + k * COV_TL_E_INTERVAL_BYTES + lane * 12, asuint(e));
        }
        // 2c. the mask and the bin ids; the slice byte
        if (lane < 16)
        {
            uint word = 0;
            [unroll] for (uint q = 0; q < 4; ++q) word |= (gs_bin[lane * 4 + q] & 0xFFu) << (8 * q);
            field.Store(tileBase + COV_TL_BINID_OFFSET + k * 64 + lane * 4, word);
        }
        if (lane == 0)
        {
            field.Store2(tileBase + COV_TL_MASK_OFFSET + k * 8, fieldable ? uint2(gs_mask[0], gs_mask[1]) : uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
            const uint wo = tileBase + COV_TL_SLICES_OFFSET + (k & ~3u);
            const uint old = field.Load(wo);  // this thread's own earlier stores (in order)
            field.Store(wo, old | (gs_slice & 0xFFu) << (8 * (k & 3)));
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
