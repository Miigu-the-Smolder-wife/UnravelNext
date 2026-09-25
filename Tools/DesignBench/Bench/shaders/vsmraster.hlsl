// Bench 5: VSM dirty-page raster (S request 2026-09-25 16:15). The page raster's information quantity is texel fragments
// (dirty pages x 128^2 + overdraw), not triangles. Two ways to write them:
//   UAV=1  the current S path: pixel shader InterlockedMax of the encoded depth into the raw page pool (page-major).
//   UAV=0  hardware depth (ROP) into a D32 atlas (page = 128^2 tile), no pixel shader; then EncodeCS copies each dirty
//          page into the pool (encoded uint) and builds the 8^2 / 32^2 block min/max hierarchy (pagemax) in one pass.
// Triangles: TRIS_PER_PAGE right triangles of edge E px per page, random position inside the page (one page each).
// P[0] = { atlas pages x, pages y, edge px (float), tris per page }, P[1] = { pool UAV (raw), seed, dispatch x, - }
// P[2] = { atlas depth SRV, blocks UAV (raw), -, - }
#include "common.hlsli"
#ifndef UAV
#define UAV 1
#endif

struct V { float4 pos : SV_Position; };
struct Prim { nointerpolation uint page : PAGE; };

[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void PageMS(uint gtid : SV_GroupThreadID, uint3 gid3 : SV_GroupID, out vertices V verts[192], out indices uint3 tris[64], out primitives Prim prims[64])
{
    SetMeshOutputCounts(192, 64);
    const uint gid = gid3.x + gid3.y * P[1].z;
    const uint pagesX = P[0].x, pagesY = P[0].y, perPage = P[0].w;
    const float W = pagesX * 128.0, H = pagesY * 128.0, E = asfloat(P[0].z);
    // Group g draws 64 triangles of page (g / groupsPerPage), groupsPerPage = perPage / 64.
    const uint groupsPerPage = max(perPage / 64, 1u);
    const uint page = (gid / groupsPerPage) % (pagesX * pagesY);
    const float2 origin = float2(page % pagesX, page / pagesX) * 128.0;
    uint seed = pcg(gid * 64u + gtid + P[1].y);
    const float2 o = origin + float2(u01(seed), u01(seed)) * (128.0 - E);
    const float z = 0.05 + 0.9 * u01(seed);
    const float2 q[3] = { o, o + float2(E, 0), o + float2(0, E) };
    [unroll] for (uint k = 0; k < 3; ++k)
        verts[gtid * 3 + k].pos = float4(q[k].x / W * 2 - 1, 1 - q[k].y / H * 2, z, 1);
    tris[gtid] = uint3(gtid * 3, gtid * 3 + 1, gtid * 3 + 2);
    prims[gtid].page = page;
}

// UAV path: encoded depth InterlockedMax into the raw pool, page-major (page * 16384 + y * 128 + x) x 4 B.
void PoolPS(V v, Prim p)
{
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[1].x];
    const uint2 t = uint2(v.pos.xy) & 127u;
    const uint addr = (p.page * 16384u + t.y * 128u + t.x) * 4u;
    pool.InterlockedMax(addr, asuint(v.pos.z));
}

// Depth-only path has no pixel shader (ROP depth test/write with GREATER).

// Encode: one group per 32 x 32 block (16 groups per page): copies the atlas depth of the page into the pool as encoded
// uint and writes the 8^2 and 32^2 block min/max (blocks buffer: page * 20 entries x 8 B: 16 of 8^2... simplified to
// 16 x (min,max) for the 8x8 blocks of the 32-block + 1 for the 32-block).
groupshared uint gsMin[64], gsMax[64];
[numthreads(256, 1, 1)]
void EncodeCS(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    Texture2D<float> atlas = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[2].y];
    const uint pagesX = P[0].x;
    const uint page = gid.x >> 4, block = gid.x & 15u;
    const uint2 pageOrigin = uint2(page % pagesX, page / pagesX) * 128u + uint2(block & 3u, block >> 2) * 32u;
    // 256 threads x 4 texels (2 x 2) = 32 x 32.
    const uint2 local = uint2((tid & 15u) * 2u, (tid >> 4) * 2u);
    uint mn = 0xFFFFFFFFu, mx = 0;
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        const uint2 t = local + uint2(j & 1u, j >> 1);
        const float d = atlas.Load(int3(pageOrigin + t, 0));
        const uint e = asuint(d);
        const uint2 tp = uint2(block & 3u, block >> 2) * 32u + t;
        pool.Store((page * 16384u + tp.y * 128u + tp.x) * 4u, e);
        mn = min(mn, e); mx = max(mx, e);
    }
    // 8 x 8 block = 16 threads (4 x 4 of 2x2); reduce via wave then groupshared.
    const uint b8 = (local.y >> 3) * 4 + (local.x >> 3);
    if (tid < 64) { gsMin[tid] = 0xFFFFFFFFu; gsMax[tid] = 0; }
    GroupMemoryBarrierWithGroupSync();
    InterlockedMin(gsMin[b8], mn);
    InterlockedMax(gsMax[b8], mx);
    GroupMemoryBarrierWithGroupSync();
    if (tid < 16) blocks.Store2((page * 17u * 16u + block * 17u + tid) * 8u, uint2(gsMin[tid], gsMax[tid]));
    if (tid == 16)
    {
        uint m0 = 0xFFFFFFFFu, m1 = 0;
        [unroll] for (uint i = 0; i < 16; ++i) { m0 = min(m0, gsMin[i]); m1 = max(m1, gsMax[i]); }
        blocks.Store2((page * 17u * 16u + block * 17u + 16u) * 8u, uint2(m0, m1));
    }
}

// Clears the pool (UAV path) to zero: 16 B per thread.
[numthreads(256, 1, 1)]
void ClearCS(uint id : SV_DispatchThreadID)
{
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[1].x];
    if (id * 16u < P[1].w) pool.Store4(id * 16u, uint4(0, 0, 0, 0));
}
