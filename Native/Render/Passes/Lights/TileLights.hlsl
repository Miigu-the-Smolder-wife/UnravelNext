// unx-kernel: cs_6_6 main
// Tile lights (RENDERER_REDESIGN_V2 14.1, 14.2; L2; owner A): one group per 8 x 8 shading tile (TileLights.hlsli for
// the record). Per tile: the receiving region (the pixels' positions: centre = the corner mean, radius = the farthest
// pixel; the normal cone of the G-buffer normals), the tile's froxel lists over the slices its depths span (the 24 px
// froxel tile holds the 8 x 8 tile: 24 = 3 x 8), each entry classified with LightNearFar.hlsli (every shadow caster
// NEAR), the FAR lights' vector irradiance summed at the four corners (each light once: an entry whose light already
// stood in an earlier slice's list adds nothing), deterministic (fixed entry -> thread assignment, fixed reduction tree).
// The record is marked invalid (every light NEAR, the exact per-pixel path) for an edge tile (EdgeDetect's mask), a
// tile with a sky corner, a depth span over more than 4 slices, or a slice list over 64 entries.
// P[0] = { depth SRV, G-buffer SRV, froxel lights SRV (this view's lists), edge tile mask SRV (R32G32_UINT) }
// P[1] = { records UAV (raw), tilesX, first tile row of this dispatch, S's tile lit records SRV (VsmCls.hlsli; UNX_NONE: every
//          caster NEAR) }
// P[2] = { the lighting channels every instance is in (GpuScene::lightingChannelsShared), 0, 0, 0 }: a light in none of
//          them is NEAR (the per-pixel path tests the pixel's instance), so the FAR sum holds no light a pixel of the
//          tile may not take.
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Common/LightNearFar.hlsli"
#include "Passes/Lights/TileLights.hlsli"
#include "Passes/Shadow/VsmCls.hlsli"

groupshared float3 gs_pos[64];
groupshared float3 gs_normal[64];
groupshared uint gs_valid[64];
groupshared float3 gs_centre, gs_axis;
groupshared float gs_radius, gs_sinCone, gs_zMin, gs_zMax;
groupshared uint gs_allNear;
groupshared uint2 gs_list[TILE_LIGHTS_SLICES];  // (first, count) per slice
groupshared uint gs_sliceFirst, gs_sliceCount, gs_total;
groupshared uint gs_mask[2 * TILE_LIGHTS_SLICES];
groupshared uint gs_near, gs_far;
groupshared float gs_sum[64][12];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const uint2 tileCoord = uint2(gid.x, gid.y + P[1].z);
    const uint2 pixel = tileCoord * 8 + tid;
    const uint tileIndex = tileCoord.y * P[1].y + tileCoord.x;
    RWByteAddressBuffer records = ResourceDescriptorHeap[P[1].x];
    const uint ro = tileIndex * TILE_LIGHTS_RECORD_BYTES;

    // 1. the pixels
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    const bool inside = pixel.x < g_viewWidth && pixel.y < g_viewHeight;
    const float depthValue = inside ? depthTex[pixel] : 0;
    const bool valid = inside && depthValue > 0;
    float3 pos = 0, n = 0;
    float linearZ = 0;
    if (valid)
    {
        linearZ = linearDepth(depthValue);
        float3 D, Dx, Dy;
        mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
        pos = D * linearZ;
        const float3 v = -normalize(D);
        n = mNormalTowardsViewer(decodeGBuffer(gbuffer[pixel]).normal, v);
    }
    gs_pos[lane] = pos;
    gs_normal[lane] = n;
    gs_valid[lane] = valid ? 1u : 0u;
    if (lane == 0)
    {
        gs_allNear = 0;
        gs_near = 0;
        gs_far = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 2 * TILE_LIGHTS_SLICES) gs_mask[lane] = 0;

    // 2. the region, the slices (thread 0; 64 entries)
    if (lane == 0)
    {
        Texture2D<uint2> edgeTiles = ResourceDescriptorHeap[P[0].w];
        const uint2 edgeMask = P[0].w != UNX_NONE ? edgeTiles[tileCoord] : uint2(0, 0);
        bool allNear = any(edgeMask != 0);
        if (!(gs_valid[0] != 0 && gs_valid[7] != 0 && gs_valid[56] != 0 && gs_valid[63] != 0)) allNear = true;
        const float3 centre = 0.25 * (gs_pos[0] + gs_pos[7] + gs_pos[56] + gs_pos[63]);
        float3 axis = 0;
        float zMin = 3.0e38, zMax = 0, radius = 0;
        for (uint i = 0; i < 64; ++i)
        {
            if (gs_valid[i] == 0) continue;
            axis += gs_normal[i];
            radius = max(radius, length(gs_pos[i] - centre));
            const float z = dot(gs_pos[i], froxelForward());
            zMin = min(zMin, z);
            zMax = max(zMax, z);
        }
        axis = length(axis) > 1e-6 ? normalize(axis) : float3(0, 1, 0);
        float minDot = 1;
        for (uint j = 0; j < 64; ++j)
            if (gs_valid[j] != 0) minDot = min(minDot, dot(gs_normal[j], axis));
        gs_centre = centre;
        gs_axis = axis;
        gs_radius = radius;
        gs_sinCone = sqrt(saturate(1 - minDot * minDot));
        gs_zMin = zMin;
        gs_zMax = zMax;
        // the froxel lists of the slices the tile spans
        FroxelSrvs f;
        f.lights = P[0].z;
        f.lightIndices = P[0].z;
        f.scattering = UNX_NONE;
        f.pad = 0;
        const FroxelGrid g = froxelGrid(f.lights);
        const uint2 froxelTile = min((tileCoord * 8) / g.tilePx, uint2(g.gridX - 1, g.gridY - 1));
        const uint s0 = zMax > 0 ? froxelSlice(g, zMin) : 0, s1 = zMax > 0 ? froxelSlice(g, zMax) : 0;
        uint count = s1 - s0 + 1, total = 0;
        if (count > TILE_LIGHTS_SLICES) allNear = true;
        else
        {
            ByteAddressBuffer b = ResourceDescriptorHeap[f.lights];
            for (uint s = 0; s < count; ++s)
            {
                const uint2 range = b.Load2(g.headerBase + froxelIndex(g, froxelTile, s0 + s) * 8);
                gs_list[s] = range;
                if (range.y > 64) allNear = true;
                total += range.y;
            }
        }
        gs_sliceFirst = s0;
        gs_sliceCount = count;
        gs_total = total;
        gs_allNear = allNear ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // 3. the entries: thread 'lane' takes entries lane, lane + 64, ...; FAR sums in registers
    float3 e0 = 0, e1 = 0, e2 = 0, e3 = 0;
    if (gs_allNear == 0)
    {
        FroxelSrvs f;
        f.lights = P[0].z;
        f.lightIndices = P[0].z;
        f.scattering = UNX_NONE;
        f.pad = 0;
        const uint indexBase = froxelIndexBase(f);
        NfRegion region;
        region.centre = g_cameraPosition + gs_centre;  // light positions are world positions
        region.halfExtent = gs_radius;
        region.normalAxis = gs_axis;
        region.normalSin = gs_sinCone;
        const float3 c0 = g_cameraPosition + gs_pos[0], c1 = g_cameraPosition + gs_pos[7], c2 = g_cameraPosition + gs_pos[56], c3 = g_cameraPosition + gs_pos[63];
        const uint sliceCount = gs_sliceCount, total = gs_total;
        VsmClsTile clsTile = (VsmClsTile)0;
        if (P[1].w != UNX_NONE) clsTile = vsmClsTile(P[1].w, tileIndex);
        for (uint e = lane; e < total; e += 64)
        {
            // slice and position of entry e
            uint s = 0, i = e;
            [loop] while (s + 1 < sliceCount && i >= gs_list[s].y) { i -= gs_list[s].y; ++s; }
            const uint li = froxelLightAt(f, indexBase, gs_list[s].x + i);
            const GpuLight light = loadLight(li);
            float d;
            // a caster lit over every pixel of the tile (L3 classification) may be FAR like an unshadowed light
            const bool litOver = P[1].w != UNX_NONE && vsmClsTileLit(clsTile, gs_sliceFirst + s, i);
            if (P[1].w != UNX_NONE && vsmClsTileUmbra(clsTile, gs_sliceFirst + s, i)) { InterlockedOr(gs_mask[s * 2 + (i >> 5)], 1u << (i & 31)); continue; }  // umbra: NEAR, 0 per pixel
            const bool near = nfIsNear(light, region, false, !lightCastsShadow(light) || litOver, d) || !nfLightsEveryInstance(light, P[2].x);
            if (near)
            {
                InterlockedOr(gs_mask[s * 2 + (i >> 5)], 1u << (i & 31));
                InterlockedAdd(gs_near, 1);
                continue;
            }
            // once per light: an earlier slice's list holding it adds it there
            bool seen = false;
            for (uint sp = 0; sp < s && !seen; ++sp)
                for (uint j = 0; j < gs_list[sp].y && !seen; ++j)
                    seen = froxelLightAt(f, indexBase, gs_list[sp].x + j) == li;
            if (seen) continue;
            e0 += nfVectorIrradiance(light, c0);
            e1 += nfVectorIrradiance(light, c1);
            e2 += nfVectorIrradiance(light, c2);
            e3 += nfVectorIrradiance(light, c3);
            InterlockedAdd(gs_far, 1);
        }
    }
    gs_sum[lane][0] = e0.x; gs_sum[lane][1] = e0.y; gs_sum[lane][2] = e0.z;
    gs_sum[lane][3] = e1.x; gs_sum[lane][4] = e1.y; gs_sum[lane][5] = e1.z;
    gs_sum[lane][6] = e2.x; gs_sum[lane][7] = e2.y; gs_sum[lane][8] = e2.z;
    gs_sum[lane][9] = e3.x; gs_sum[lane][10] = e3.y; gs_sum[lane][11] = e3.z;
    GroupMemoryBarrierWithGroupSync();
    // 4. deterministic reduction tree
    [unroll] for (uint step = 32; step > 0; step >>= 1)
    {
        if (lane < step)
            [unroll] for (uint k = 0; k < 12; ++k) gs_sum[lane][k] += gs_sum[lane + step][k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        const bool valid = gs_allNear == 0;
        float sum[12];
        [unroll] for (uint k = 0; k < 12; ++k) sum[k] = valid ? gs_sum[0][k] : 0;
        records.Store4(ro, uint4(asuint(sum[0]), asuint(sum[1]), asuint(sum[2]), asuint(sum[3])));
        records.Store4(ro + 16, uint4(asuint(sum[4]), asuint(sum[5]), asuint(sum[6]), asuint(sum[7])));
        records.Store4(ro + 32, uint4(asuint(sum[8]), asuint(sum[9]), asuint(sum[10]), asuint(sum[11])));
        const uint flags = (valid ? 1u : 0u) | (gs_sliceFirst & 0xFFu) << 8 | (gs_sliceCount & 0xFFu) << 16;
        records.Store2(ro + 48, uint2(flags, gs_near | gs_far << 16));
        records.Store4(ro + 56, uint4(gs_mask[0], gs_mask[1], gs_mask[2], gs_mask[3]));
        records.Store4(ro + 72, uint4(gs_mask[4], gs_mask[5], gs_mask[6], gs_mask[7]));
        records.Store2(ro + 88, uint2(0, 0));
    }
}
