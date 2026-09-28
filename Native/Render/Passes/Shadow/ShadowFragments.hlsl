// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Fragment shadow visibility for the coverage layer (S request 20260926_S_fragment_visibility; INTERFACES 7.3 v1.41;
// COVERAGE_REDESIGN 4.3): the sun and local-light visibility of the thin fragments (grass blades, leaves, wires) that sit
// in front of a pixel's band A surface, for M's coverage composite.
//   MODE 0, one group per listed coverage tile (V's tile list args), one thread per pixel with records: the segment of
//     the pixel's view ray between its nearest and farthest record (ViewResources::coverageDepthRange) is classified
//     exactly against the block hierarchy (vsmSegmentClassify: every point of it lit, or none): lit -> sun visibility
//     255 x the thin casters' T at 4 points z_k = z_near + (z_far - z_near) k / 3 (linear view depth), umbra -> 0;
//     otherwise the pixel's pair flag is set and MODE 1 evaluates each of its records. Local slots 1-3 (the pixel's
//     froxel list at each end's depth) with the pixel receiver function at z_near (y) and z_far (z).
//   MODE 1, one group per block of COV_BLOCK records (V's block args; fixed work per group): each record of a pair-flag
//     pixel gets the SMRT estimator with its own normal's receiver plane (vsmSunVisibility) x T, one byte in
//     shadowFragmentSun at its record element index; bytes of other records are not written.
// Output (ViewResources::shadowFragmentVisibility, uint3 per pixel y x width + x): x = 4 x unorm8 sun, y = byte 0 bit 0
// pair flag, bytes 1..3 local slots 1..3 at z_near (7.3 encoding, 255 = no slot), z = bytes 1..3 at z_far.
// P[0] = { depth range SRV (R32G32_UINT), tile list SRV (raw), records SRV (StructuredBuffer<uint4>), output UAV }
// P[1] = { VSM page table SRV, atlas SRV, search bound SRV, blocks SRV }
// P[2] = { VSM constants CBV, local lights SRV, slot of light SRV, froxel lists SRV (0xFFFFFFFF: none) }
// P[3] = { fragment sun UAV (raw), layers SRV (0xFFFFFFFF: none), band A depth SRV, G-buffer SRV }
// P[4] = { VSM stats UAV (raw: words 25 pixels with records, 26 pair pixels, 27..29 the check below), depth range SRV
//          (MODE 1, for the check) }
// shadow.vsm.fragment_check (VsmConstants::fragmentCheck, verification only): MODE 1 evaluates every record; for records of
// settled pixels it compares the per-record SMRT with the value M reads (the 4-point sun bytes interpolated at the
// record's linear depth) and counts differences above 1/255 (words 27 checked, 28 mismatches, 29 largest x 255).
// Frame constants of the view.
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

ShadowSrvs fragmentSrvs()
{
    ShadowSrvs s;
    s.pageTable = P[1].x;
    s.pool = P[1].y;
    s.searchBound = P[1].z;
    s.blocks = P[1].w;
    s.constants = P[2].x;
    s.lights = P[2].y;
    s.pad0 = P[2].z;
    s.layers = P[3].y;
    return s;
}
VsmResources fragmentVsm()
{
    VsmResources r;
    r.table = ResourceDescriptorHeap[P[1].x];
    r.pool = ResourceDescriptorHeap[P[1].y];
    r.searchBound = ResourceDescriptorHeap[P[1].z];
    r.blocks = ResourceDescriptorHeap[P[1].w];
    r.cbv = P[2].x;
    return r;
}
float pixelFootprint(float deviceDepth) { return 2 * linearDepth(deviceDepth) * g_tanHalfFovY / g_viewHeight; }
float fragmentSunT(ShadowSrvs s, float3 p, float footprint) { return P[3].y != 0xFFFFFFFFu ? shadowSunTransmittanceAt(s, p, footprint, footprint) : 1.0; }

#if MODE == 0
// Local slots 1-3 (bytes 1..3; 255 = no slot) of the pixel's froxel list at view depth z, receiver at p with normal n.
uint localSlots(ShadowSrvs s, uint2 px, float3 p, float3 n, float deviceDepth)
{
    uint local = 0xFFFFFF00u;
    if (P[2].w == 0xFFFFFFFFu) return local;
    FroxelSrvs f;
    f.lights = P[2].w;
    f.lightIndices = P[2].w;
    f.scattering = 0;
    f.pad = 0;
    const uint2 range = froxelLightRange(f, px, linearDepth(deviceDepth));
    const uint indexBase = froxelIndexBase(f);
    ShadowPixelReceiver pr;
    pr.world = p;
    pr.normal = n;
    pr.footprint = pixelFootprint(deviceDepth);
    pr.valid = 1;
    uint ordinal = 0;
    [loop] for (uint i = 0; i < range.y && ordinal < 3; ++i)
    {
        const uint li = froxelLightAt(f, indexBase, range.x + i);
        if (!lightCastsShadow(loadLight(li))) continue;
        ++ordinal;
        const float v = shadowLocalVisibilityAtReceiver(s, li, pr);
        local = (local & ~(0xFFu << (8 * ordinal))) | ((uint)round(saturate(v) * 255.0) << (8 * ordinal));
    }
    return local;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint j = gid.y * 65535u + gid.x;
    if (j >= list.Load(COV_LIST_COUNT * 4)) return;
    const uint tile = list.Load((COV_LIST_INFO + 4 * j) * 4), tilesX = list.Load(COV_LIST_TILES_X * 4);
    const uint2 px = uint2(tile % tilesX, tile / tilesX) * COV_TILE_PX + uint2(lane % COV_TILE_PX, lane / COV_TILE_PX);
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<uint2> ranges = ResourceDescriptorHeap[P[0].x];
    const uint2 range = ranges.Load(int3(px, 0));
    if (range.x == 0 && range.y == 0xFFFFFFFFu) return;  // no records: M reads nothing here
    const float dNear = asfloat(range.x), dFar = asfloat(range.y);
    const float3 pNear = worldFromDepth(float2(px), dNear), pFar = worldFromDepth(float2(px), dFar);
    const float footprint = pixelFootprint(dNear);
    // The band A surface the fragments stand on (its plane settles fragments lying on it; none: free air).
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[3].z];
    const float dSurface = depthTex.Load(int3(px, 0));
    float3 surfaceNormal = 0, surfacePoint = pNear;
    if (dSurface > 0) surfacePoint = shadowReceiver(depthTex, P[3].w, px, dSurface, surfaceNormal);
    const ShadowSrvs s = fragmentSrvs();
    const uint cls = vsmFragmentSegmentClassify(fragmentVsm(), pNear, pFar, linearDepth(dNear), linearDepth(dFar), 2 * g_tanHalfFovY / g_viewHeight,
                                                tan(g_sunAngularRadius), surfacePoint, surfaceNormal);
    uint sun = 0, pair = 0;
    if (cls == VSM_REGION_LIT)
    {
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const float z = lerp(linearDepth(dNear), linearDepth(dFar), k / 3.0);
            const float3 p = lerp(pNear, pFar, (z - linearDepth(dNear)) / max(linearDepth(dFar) - linearDepth(dNear), 1e-30));
            sun |= (uint)round(saturate(fragmentSunT(s, p, footprint)) * 255.0) << (8 * k);
        }
    }
    else if (cls == VSM_REGION_MIXED)
        pair = 1;
    // Local receivers: the band A surface's normal where the fragments stand on it, else facing the camera.
    const float3 n = dSurface > 0 ? surfaceNormal : normalize(g_cameraPosition - pNear);
    const uint localNear = localSlots(s, px, pNear, n, dNear), localFar = localSlots(s, px, pFar, n, dFar);
    RWStructuredBuffer<uint3> output = ResourceDescriptorHeap[P[0].w];
    output[px.y * g_viewWidth + px.x] = uint3(sun, (localNear & 0xFFFFFF00u) | pair, localFar & 0xFFFFFF00u);
    const uint pixels = WaveActiveCountBits(true), pairs = WaveActiveCountBits(pair != 0);
    if (WaveIsFirstLane())
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[4].x];
        stats.InterlockedAdd(100, pixels);
        if (pairs) stats.InterlockedAdd(104, pairs);
    }
}
#else
#define FRAG_THREADS 256u
[numthreads(FRAG_THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint block = gid.y * 65535u + gid.x;
    if (block >= list.Load(COV_LIST_BLOCKS * 4)) return;
    // The listed tile holding this block: the last j whose block base <= block (block bases are an exclusive prefix
    // over the list; binary search, at most 32 steps).
    uint lo = 0, hi = list.Load(COV_LIST_COUNT * 4);
    [loop] for (uint step = 0; step < 32 && hi - lo > 1; ++step)
    {
        const uint mid = (lo + hi) / 2;
        if (list.Load((COV_LIST_INFO + 4 * mid + 3) * 4) <= block) lo = mid;
        else hi = mid;
    }
    const uint4 info = list.Load4((COV_LIST_INFO + 4 * lo) * 4);  // tile, records, record base, block base
    const uint tilesX = list.Load(COV_LIST_TILES_X * 4);
    const uint2 tileOrigin = uint2(info.x % tilesX, info.x / tilesX) * COV_TILE_PX;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint3> pixels = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer sunBytes = ResourceDescriptorHeap[P[3].x];
    const ShadowSrvs s = fragmentSrvs();
    const VsmResources r = fragmentVsm();
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[2].x];
    const uint first = (block - info.w) * COV_BLOCK;
    const bool check = vc.fragmentCheck != 0;
    uint checked = 0, mismatched = 0, largest = 0;
    [unroll] for (uint i = 0; i < COV_BLOCK / FRAG_THREADS; ++i)
    {
        const uint local = first + lane + i * FRAG_THREADS;
        if (local >= info.y) continue;
        const uint element = info.z + local;
        const uint4 raw = records[element];
        CoverageFragment f;
        f.visId = raw.x;
        f.depthBits = raw.y;
        f.mask = raw.z;
        f.packed = raw.w;
        const uint p = coverageFragmentPixel(f);
        const uint2 px = tileOrigin + uint2(p % COV_TILE_PX, p / COV_TILE_PX);
        if (px.x >= g_viewWidth || px.y >= g_viewHeight) continue;
        const uint3 settled = pixels[px.y * g_viewWidth + px.x];
        const bool pairPixel = (settled.y & 1u) != 0;
        if (!pairPixel && !check) continue;  // settled by MODE 0
        const float d = coverageFragmentDepth(f);
        const float3 world = worldFromDepth(float2(px), d);
        const float footprint = pixelFootprint(d);
        uint path;
        const float v = vsmSunVisibility(r, world, coverageFragmentNormal(f), footprint, vc.tanSunRadius, vc.searchTaps, vc.filterTaps, path) *
                        fragmentSunT(s, world, footprint);
        if (!pairPixel)
        {
            // The value M reads for a settled pixel: the 4-point bytes interpolated at the record's linear depth.
            Texture2D<uint2> ranges = ResourceDescriptorHeap[P[4].y];
            const uint2 range = ranges.Load(int3(px, 0));
            const float zn = linearDepth(asfloat(range.x)), zf = linearDepth(asfloat(range.y)), z = linearDepth(d);
            const float x = zf > zn ? saturate((z - zn) / (zf - zn)) * 3 : 0;
            const uint i0 = min((uint)x, 2u);
            const float b0 = (settled.x >> (8 * i0)) & 0xFFu, b1 = (settled.x >> (8 * (i0 + 1))) & 0xFFu;
            const float m = lerp(b0, b1, x - i0);
            const float diff = abs(m - saturate(v) * 255.0);
            ++checked;
            if (diff > 1.0)
            {
                ++mismatched;
                // The first mismatch for diagnosis: words 30 = pixel + 1, 31 = settled | per-record << 8 | segment point x 64 << 16.
                RWByteAddressBuffer stats = ResourceDescriptorHeap[P[4].x];
                uint was;
                stats.InterlockedCompareExchange(120, 0u, ((px.y << 16) | px.x) + 1, was);
                if (was == 0) stats.Store(124, (uint)round(m) | ((uint)round(saturate(v) * 255.0) << 8) | ((uint)round(x * 64) << 16));
            }
            largest = max(largest, (uint)ceil(diff));
            continue;
        }
        const uint shift = 8 * (element & 3u), word = (element & ~3u);
        sunBytes.InterlockedAnd(word, ~(0xFFu << shift));
        sunBytes.InterlockedOr(word, (uint)round(saturate(v) * 255.0) << shift);
    }
    if (check)
    {
        const uint c = WaveActiveSum(checked), mm = WaveActiveSum(mismatched), lg = WaveActiveMax(largest);
        if (WaveIsFirstLane())
        {
            RWByteAddressBuffer stats = ResourceDescriptorHeap[P[4].x];
            stats.InterlockedAdd(108, c);
            stats.InterlockedAdd(112, mm);
            stats.InterlockedMax(116, lg);
        }
    }
}
#endif
