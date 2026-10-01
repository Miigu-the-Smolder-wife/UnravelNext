// unx-kernel: cs_6_6 main
// (Tile, light) classification over the classification pages (RENDERER_REDESIGN_V2 14.3-2, L3 stage 1; owner A): one
// group per 8 x 8 tile of the main view, one thread per pixel. For every entry of the froxel lists of the slices the
// tile's depths span (<= 4) whose light has a shadow slot, each pixel projects its surface point onto the light's cube
// (VsmLocal.hlsli) and reads the block of the classification page holding its texel: the pixel is lit when no caster
// in that block is nearer than the pixel (block max <= the pixel's reversed-Z depth plus the tolerance: the pixel's own
// surface is a caster of the same depth) and the pixel is beyond the near plane. A point light's occluders lie on the
// pixel's ray, so its texel's block holds them all (the conservative raster missed none); an area light of radius r_L
// needs the blocks around it too: its emitter subtends 2 r_L / z, the 3 x 3 blocks around the texel cover 24 texels of
// the 128-texel face (0.19 of the tangent range), so a light with r_L / z <= 0.08 takes the 3 x 3 blocks and a wider
// one is never lit here. The tile's bit is the AND over its surface pixels (sky pixels do not vote). Lit is exact;
// everything else stays mixed (umbra needs the exact-raster twin, 14.3-1, stage 2).
// P[0] = { depth SRV, froxel lights SRV, local lights SRV (StructuredBuffer<VsmLocalLight>), slot of light SRV }
// P[1] = { classification blocks SRV (raw), tile lit records UAV (raw), tilesX, tolerance bits (float: relative depth) }
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"
#include "Passes/Shadow/VsmCls.hlsli"

groupshared uint gs_lit[2 * VSM_CLS_TILE_SLICES];
groupshared uint2 gs_list[VSM_CLS_TILE_SLICES];
groupshared uint gs_first, gs_count, gs_valid;
groupshared uint gs_zMinBits, gs_zMaxBits;

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const uint2 tileCoord = gid.xy;
    const uint2 pixel = tileCoord * 8 + tid;
    const uint tileIndex = tileCoord.y * P[1].z + tileCoord.x;
    RWByteAddressBuffer records = ResourceDescriptorHeap[P[1].y];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const bool inside = pixel.x < g_viewWidth && pixel.y < g_viewHeight;
    const float depthValue = inside ? depthTex[pixel] : 0;
    const bool surface = inside && depthValue > 0;
    float linearZ = 0;
    float3 worldPos = 0;
    if (surface)
    {
        linearZ = linearDepth(depthValue);
        float3 D, Dx, Dy;
        mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
        worldPos = g_cameraPosition + D * linearZ;
    }
    if (lane == 0)
    {
        gs_zMinBits = 0x7F800000u;
        gs_zMaxBits = 0;
        gs_valid = 0;
        [unroll] for (uint k = 0; k < 2 * VSM_CLS_TILE_SLICES; ++k) gs_lit[k] = 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();
    if (surface)
    {
        InterlockedMin(gs_zMinBits, asuint(linearZ));
        InterlockedMax(gs_zMaxBits, asuint(linearZ));
    }
    GroupMemoryBarrierWithGroupSync();
    FroxelSrvs f;
    f.lights = P[0].y;
    f.lightIndices = P[0].y;
    f.scattering = UNX_NONE;
    f.pad = 0;
    const FroxelGrid g = froxelGrid(f.lights);
    if (lane == 0)
    {
        bool valid = gs_zMaxBits != 0;
        uint first = 0, count = 0;
        if (valid)
        {
            const float zMin = asfloat(gs_zMinBits), zMax = asfloat(gs_zMaxBits);
            first = froxelSlice(g, zMin);
            count = froxelSlice(g, zMax) - first + 1;
            if (count > VSM_CLS_TILE_SLICES) valid = false;
            else
            {
                ByteAddressBuffer b = ResourceDescriptorHeap[f.lights];
                const uint2 froxelTile = min((tileCoord * 8) / g.tilePx, uint2(g.gridX - 1, g.gridY - 1));
                for (uint s = 0; s < count; ++s)
                {
                    const uint2 range = b.Load2(g.headerBase + froxelIndex(g, froxelTile, first + s) * 8);
                    gs_list[s] = range;
                    if (range.y > 64) valid = false;
                }
            }
        }
        gs_valid = valid ? 1u : 0u;
        gs_first = first;
        gs_count = count;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gs_valid != 0 && surface)
    {
        const uint indexBase = froxelIndexBase(f);
        StructuredBuffer<VsmLocalLight> locals = ResourceDescriptorHeap[P[0].z];
        StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].w];
        ByteAddressBuffer blocks = ResourceDescriptorHeap[P[1].x];
        const float tolerance = asfloat(P[1].w);
        for (uint s = 0; s < gs_count; ++s)
        {
            const uint2 range = gs_list[s];
            for (uint i = 0; i < range.y; ++i)
            {
                const uint li = froxelLightAt(f, indexBase, range.x + i);
                const uint slot = li < g_lightCount ? slotOf[li] : VSM_LOCAL_NONE;
                bool lit = false;
                if (slot != VSM_LOCAL_NONE)
                {
                    const VsmLocalLight l = locals[slot];
                    const VsmLocalPoint q = vsmLocalProject(l, worldPos);
                    if (q.z > l.nearM && l.radius <= 0.08 * q.z)
                    {
                        // the receiver's reversed-Z device depth on this face (VsmSystem.cpp localViewProj)
                        const float dr = l.nearM * (l.farM - q.z) / ((l.farM - l.nearM) * q.z);
                        const float limit = saturate(dr * (1 + tolerance));
                        const int2 t = clamp(int2(floor(vsmLocalTexel(q.xy, 0))), 0, 127);
                        const int2 blk = t / 8;
                        const int reach = l.radius > 0 ? 1 : 0;
                        const uint page = l.activeIndex * 6 + q.face;
                        float nearest = 0;
                        for (int dy = -reach; dy <= reach; ++dy)
                            for (int dx = -reach; dx <= reach; ++dx)
                            {
                                const int2 c = clamp(blk + int2(dx, dy), 0, 15);
                                nearest = max(nearest, asfloat(blocks.Load((page * 256 + c.y * 16 + c.x) * 4)));
                            }
                        lit = nearest <= limit;
                    }
                }
                if (!lit) InterlockedAnd(gs_lit[s * 2 + (i >> 5)], ~(1u << (i & 31)));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        const uint o = tileIndex * VSM_CLS_TILE_BYTES;
        const bool valid = gs_valid != 0;
        records.Store4(o, uint4(valid ? 1u : 0u, gs_first, gs_count, 0));
        // slices the tile does not span: nothing lit
        [unroll] for (uint s = 0; s < VSM_CLS_TILE_SLICES; ++s)
        {
            const bool spanned = valid && s < gs_count;
            records.Store2(o + 16 + s * 8, spanned ? uint2(gs_lit[s * 2], gs_lit[s * 2 + 1]) : uint2(0, 0));
        }
    }
}
