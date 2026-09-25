// unx-kernel: cs_6_6 main
// unx-variants: PATHS=0,1
// Shadow visibility, pass 1 of 2 (ARCHITECTURE 2.11: small kernels separate from shading, so the page-table and pool
// dependent loads are hidden by occupancy). Writes 4 B per pixel (INTERFACES 7.3): slot 0 = sun, slots 1-3 = the first
// three shadow-casting local lights of the pixel's froxel list, in list order (vsmLocalVisibility; 255 for a casting light
// without a shadow slot, and in views without froxel lists). Settles every pixel whose
// sun visibility the page structures decide exactly (vsmSunClassify: no caster within reach, the block hierarchy over
// the reach square all below or all above the receiver's plane) and compacts the rest into a list for pass 2
// (ShadowPenumbra.hlsl, indirect).
// PATHS=1 (diagnostics): writes each pixel's VSM_PATH_* (0xFF = sky) instead of the visibility.
// Shadow-casting lights past the third (INTERFACES 7.3 overflow list, v1.20): each pixel counts them (list only, no VSM
// taps); a tile (= this 8x8 group = M's shading tile) without any writes its shadowOverflowTiles head 0, a tile with some
// goes to the overflow tile list, whose tiles ShadowOverflow.hlsl allocates and evaluates.
// P[0].x depth SRV, P[0].y G-buffer SRV (RG32_UINT), P[0].z output UAV (R32_UINT), P[0].w VSM constants CBV
// P[1].x overflow tile list UAV (raw: count, dispatch args, tiles y << 16 | x; 0xFFFFFFFF: no overflow list in this view),
// P[1].y page table SRV (raw), P[1].z pool SRV (raw), P[1].w search bound SRV (raw)
// P[2].x penumbra list UAV (raw: count, then pixel y << 16 | x), P[2].y blocks SRV (raw), P[2].z statistics UAV (raw,
// words 8.. of the VSM stats: pixels per VSM_PATH_*)
// P[3].x froxel lists SRV (raw; 0xFFFFFFFF: no local slots in this view), P[3].y local lights SRV, P[3].z slot of light SRV,
// P[3].w overflow tile heads UAV (R32_UINT, with P[1].x).
// P[4].x planar mask SRV (R8_UINT per pixel), P[4].y planar tile mask SRV (R8_UINT per 8 x 8 tile = this group)
// (planar reflection views, v1.22; 0xFFFFFFFF: every pixel): tiles without mirror pixels are skipped whole (head 0),
// pixels that are not mirror pixels are left as sky (M shades neither). P[4].z transmittance layer SRV (raw,
// FrameResources::vsmLayers; 0xFFFFFFFF: none): slot 0 = opaque visibility x the thin casters' T (v1.26).
// Frame constants of the view. Mixed pixels get their local slots here and their sun slot in pass 2.
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"

// One pixel's classification: sky, settled (packed visibility), or mixed (goes to pass 2).
void classifyPixel(uint2 px, out uint packed, out uint path, out bool mixed, out uint overflow)
{
    packed = 0xFFFFFFFFu;
    path = 0xFFu;  // sky
    mixed = false;
    overflow = 0;
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    if (P[4].x != 0xFFFFFFFFu)
    {
        Texture2D<uint> mask = ResourceDescriptorHeap[P[4].x];
        if (mask.Load(int3(px, 0)) == 0) return;
    }
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0) return;
    VsmResources r;
    r.table = ResourceDescriptorHeap[P[1].y];
    r.pool = ResourceDescriptorHeap[P[1].z];
    r.searchBound = ResourceDescriptorHeap[P[1].w];
    r.blocks = ResourceDescriptorHeap[P[2].y];
    r.cbv = P[0].w;
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[0].w];
    float3 normal;
    const float3 world = shadowReceiver(depthTex, P[0].y, px, depth, normal);
    const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
    const VsmReceiver rc = vsmMakeReceiver(vc, world, normal);
    uint k;
    float reach;
    const uint cls = vsmSunClassify(r, rc, footprint, tan(g_sunAngularRadius), k, reach, path);
    mixed = cls == VSM_REGION_MIXED;
    // Slots 1-3: the first three shadow-casting lights of the pixel's froxel list (INTERFACES 7.3).
    uint local = 0xFFFFFF00u;
    if (P[3].x != 0xFFFFFFFFu)
    {
        FroxelSrvs f;
        f.lights = P[3].x;
        f.lightIndices = P[3].x;
        f.scattering = 0;
        f.pad = 0;
        const uint2 range = froxelLightRange(f, px, linearDepth(depth));
        ShadowSrvs ss;
        ss.pageTable = P[1].y;
        ss.pool = P[1].z;
        ss.blocks = P[2].y;
        ss.searchBound = P[1].w;
        ss.constants = P[0].w;
        ss.lights = P[3].y;
        ss.pad0 = P[3].z;
        ss.pad1 = 0xFFFFFFFFu;
        ShadowPixelReceiver pr;
        pr.world = world;
        pr.normal = normal;
        pr.footprint = footprint;
        pr.valid = 1;
        uint ordinal = 0;
        [loop] for (uint i = 0; i < range.y; ++i)
        {
            const uint li = froxelLight(f, range.x + i);
            if (!lightCastsShadow(loadLight(li))) continue;
            ++ordinal;
            if (ordinal > 3) continue;  // counted for the overflow list (ShadowOverflow.hlsl evaluates it)
            // No shadow slot (more than 128 casting lights): 1, stored 255.
            const float v = shadowLocalVisibilityAtReceiver(ss, li, pr);
            local = (local & ~(0xFFu << (8 * ordinal))) | ((uint)round(saturate(v) * 255.0) << (8 * ordinal));
        }
        overflow = ordinal > 3 ? ordinal - 3 : 0;
    }
    // Thin casters (transmittance layer, v1.26): T at the receiver over the reach of its settled class.
    float sunT = 1;
    if (cls != VSM_REGION_UMBRA && P[4].z != 0xFFFFFFFFu)
    {
        ShadowSrvs ts;
        ts.pageTable = P[1].y;
        ts.pool = P[1].z;
        ts.blocks = P[2].y;
        ts.searchBound = P[1].w;
        ts.constants = P[0].w;
        ts.lights = P[3].y;
        ts.pad0 = P[3].z;
        ts.pad1 = P[4].z;
        sunT = shadowSunTransmittanceAt(ts, world, footprint, max(reach, footprint));
    }
    packed = (cls == VSM_REGION_UMBRA ? 0u : (uint)round(saturate(sunT) * 255.0)) | local;
}

groupshared uint gs_overflow;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].z];
    uint packed[1], path[1], overflow[1];
    bool mixed[1];
    if (P[4].y != 0xFFFFFFFFu)
    {
        Texture2D<uint> tiles = ResourceDescriptorHeap[P[4].y];
        if (tiles.Load(int3(gid.xy, 0)) == 0)  // no mirror pixel in this 8 x 8 tile (group-uniform)
        {
            if (gi == 0 && P[1].x != 0xFFFFFFFFu)
            {
                RWTexture2D<uint> heads = ResourceDescriptorHeap[P[3].w];
                heads[gid.xy] = 0;
            }
            return;
        }
    }
    if (gi == 0) gs_overflow = 0;
    classifyPixel(id.xy, packed[0], path[0], mixed[0], overflow[0]);
    if (P[1].x != 0xFFFFFFFFu)
    {
        // The tile's overflow: head 0 now, or the tile to the overflow list (its head is ShadowOverflow.hlsl's).
        GroupMemoryBarrierWithGroupSync();
        if (overflow[0]) InterlockedOr(gs_overflow, 1u);
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0)
        {
            if (gs_overflow == 0)
            {
                RWTexture2D<uint> heads = ResourceDescriptorHeap[P[3].w];
                heads[gid.xy] = 0;
            }
            else
            {
                RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
                uint at;
                tiles.InterlockedAdd(0, 1, at);
                tiles.InterlockedAdd(4, 1);  // dispatch args: one group per tile
                tiles.Store(16 + at * 4, (gid.y << 16) | gid.x);
            }
        }
    }
    {
        const uint j2 = 0;
        const uint2 px = id.xy;
        // Compact the mixed pixels (one atomic per wave).
        const uint n = WaveActiveCountBits(mixed[j2]);
        uint base = 0;
        if (WaveIsFirstLane() && n) list.InterlockedAdd(0, n, base);
        base = WaveReadLaneFirst(base);
        if (mixed[j2]) list.Store(4 + (base + WavePrefixCountBits(mixed[j2])) * 4, (px.y << 16) | px.x);
        if (px.x < g_viewWidth && px.y < g_viewHeight && (!mixed[j2] || !PATHS))
        {
#if PATHS
            output[px] = path[j2];
#else
            output[px] = packed[j2];  // mixed: the local slots now, the sun slot in pass 2
#endif
        }
        [unroll] for (uint i = 0; i < 3; ++i)
        {
            const uint c = WaveActiveCountBits(path[j2] == i);
            if (WaveIsFirstLane() && c) stats.InterlockedAdd(32 + i * 4, c);
        }
    }
}
