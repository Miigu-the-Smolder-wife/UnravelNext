// unx-kernel: cs_6_6 main
// G grid jobs (ReflectionClassify's header), one group per 8 x 8 tile after the classification. Every G pixel p keeps the
// spacing s_p = 2^L it wants and interpolates the corners of its cell on the grid of multiples of s_p (ReflectionResolve).
// A pixel q is a job when, for some level L, q is a multiple of 2^L and a G pixel within 2^L of q (per axis, exclusive)
// wants 2^L: every corner some pixel interpolates is then a job, whatever its neighbours want (the grids are nested:
// multiples of 2^L are multiples of every finer spacing). Only the jobs this needs are added to the grids the pixels
// want. The wanted spacings of the tile and its 8 neighbours (24 x 24 px, s <= 8) are read into groupshared; tiles
// ReflectionClassify did not mark have no G pixel. Own-job pixels (REFL_SELF) keep their job and count as wanting their
// spacing (ReflectionResolve tests whether that grid would serve them again).
// P[0] = { modes UAV, jobs UAV, counter UAV (uint at 0; G jobs at 8), 0 }, P[1] = { width, height, tilesX, tilesY },
// P[2].x = reflection UAV (tile validity texels at rows P[2].y + tile.y).
#include "Passes/Reflection/ReflectionInternal.hlsli"

#define JOBS_NONE 0xFu
groupshared uint gs_want[24 * 24];  // log2 of the spacing a G pixel wants, JOBS_NONE otherwise
groupshared uint gs_tiles[9];      // the nine validity flags, once per group instead of once per pixel

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[2].x];
    if (reflection[uint2(tile.x, P[2].y + tile.y)].a < 0.5) return;  // all K (or planar only): no G pixel
    const uint2 size = P[1].xy, tiles = P[1].zw;
    RWTexture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    if (lane < 9)
    {
        const int2 at = int2(tile) + int2(lane % 3, lane / 3) - 1;
        gs_tiles[lane] = all(at >= 0) && all(at < int2(tiles)) ?
            (reflection[uint2(at.x, P[2].y + at.y)].a >= 0.5 ? 1u : 0u) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    // The 3 x 3 tiles' wanted spacings (9 texels per thread).
    for (uint i = lane; i < 24 * 24; i += 64)
    {
        const int2 a = int2(tile) * 8 - 8 + int2(i % 24, i / 24);
        uint want = JOBS_NONE;
        if (all(a >= 0) && all(a < int2(size)))
        {
            if (gs_tiles[(i / 24 / 8) * 3 + (i % 24 / 8)] != 0)
            {
                const uint m = modes[uint2(a)];
                if (reflMode(m) == REFL_G) want = (m >> 2) & 7u;
            }
        }
        gs_want[i] = want;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = tile * 8 + local;
    const uint me = gs_want[(local.y + 8) * 24 + local.x + 8];
    bool job = false;
    if (all(pixel < size) && me != JOBS_NONE)
    {
        const uint m = modes[pixel];
        if ((m & REFL_SELF) == 0)
            [unroll] for (uint L = 0; L <= 3 && !job; ++L)
            {
                const uint s = 1u << L;
                if (any((pixel % s) != 0)) break;  // not on this grid, nor on any coarser one
                const int r = (int)s - 1;           // pixels within s (exclusive) of q per axis
                for (int dy = -r; dy <= r && !job; ++dy)
                    for (int dx = -r; dx <= r && !job; ++dx)
                        job = gs_want[(local.y + 8 + dy) * 24 + local.x + 8 + dx] == L;
            }
    }
    // Only G pixels are jobs (a corner that is not G - another surface, K, M - is not interpolated: ReflectionResolve).
    const uint count = WaveActiveCountBits(job);
    RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].z];
    uint base = 0;
    if (WaveIsFirstLane() && count > 0)
    {
        counter.InterlockedAdd(0, count, base);
        counter.InterlockedAdd(8, count);  // statistics: G jobs
    }
    base = WaveReadLaneFirst(base);
    const uint index = job ? base + WavePrefixCountBits(job) : REFL_NO_JOB;  // every lane (wave op before the writes)
    if (job)
    {
        RWStructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].y];
        jobs[index] = reflPackPixel(pixel);
        modes[pixel] = (modes[pixel] & 0xFFu) | (index << 8);
    }
}
