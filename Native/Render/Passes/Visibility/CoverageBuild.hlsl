// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4
// Coverage layer build (CoverageLayer.hlsli root constants):
//   MODE=0 (1 thread, after culling): DispatchMesh args over the band B list (both phases) and the clear args over last
//          frame's coverage pixels.
//   MODE=1 (per last frame's coverage pixel): its head to 0 (every other head is already 0).
//   MODE=2 (1 thread, after the raster): args over this frame's coverage pixels; the pixel buffer header for M.
//   MODE=3 (per coverage pixel): walks the pixel's linked fragments, allocates a contiguous range of at most 255 (more
//          sets OVERFLOW_COVERAGE_DEPTH and keeps the first 255 of the walk), writes them sorted nearest first (larger
//          reversed-Z depth first, then larger vis id: deterministic whatever the append order) without duplicates of a
//          vis id (clipped primitives, below) and sets the head to first | count << 24; adds the written count to the
//          header. Up to 8 fragments sort in registers (19-comparator network); more sort in place.
//   MODE=4 (2D, 8 x 8 groups over the view; on creation): heads to 0; thread (0, 0) zeroes the pixel buffer header.
#include "Passes/Visibility/CoverageLayer.hlsli"

#define WALK_LIMIT 1024u  // bound on a list walk (a list never holds more than the frame's fragments)

void storeDispatch64(RWByteAddressBuffer args, uint word, uint items)
{
    const uint groups = (items + 63) / 64;
    args.Store3(4 * word, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1));
}

uint itemOf(uint3 gid, uint lane) { return (gid.x + gid.y * 65535u) * 64u + lane; }

// Nearest first: larger depth, then larger vis id.
bool nearer(CoverageFragment x, CoverageFragment y) { return x.depth > y.depth || (x.depth == y.depth && x.visId > y.visId); }

CoverageFragment toSorted(CoverageRawFragment r)
{
    CoverageFragment f;
    f.visId = r.visId;
    f.depth = r.depth;
    f.coverageMask32 = r.mask;
    f.area = r.area;
    return f;
}

#if MODE == 0 || MODE == 2
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_ARGS];
    RWByteAddressBuffer pixels = ResourceDescriptorHeap[COV_PIXELS];
#if MODE == 0
    const uint entries = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), COV_LIST_CAPACITY);
    args.Store3(4 * VA_COV_MESH, uint3(min(entries, 65535u), (entries + 65534u) / 65535u, 1));
    storeDispatch64(args, VA_COV_RESET, min(pixels.Load(4 * COV_PIXEL_COUNT), COV_CAP_PIXELS));
#else
    const uint count = min(state.Load(4 * VS_COV_PIXELS), COV_CAP_PIXELS);
    storeDispatch64(args, VA_COV_PIXELS, count);
    storeDispatch64(pixels, COV_PIXEL_ARGS, count);
    pixels.Store(4 * COV_PIXEL_COUNT, count);
    pixels.Store(4 * COV_PIXEL_FRAGMENTS, 0);  // MODE 3 adds the written fragments
#endif
}
#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer pixels = ResourceDescriptorHeap[COV_PIXELS];
    const uint i = itemOf(gid, lane);
    if (i >= min(pixels.Load(4 * COV_PIXEL_COUNT), COV_CAP_PIXELS)) return;
    RWTexture2D<uint> heads = ResourceDescriptorHeap[COV_HEADS];
    heads[coverageUnpack(pixels.Load(4 * (COV_PIXEL_LIST + i)))] = 0;
}
#elif MODE == 3
#define SORT_LOCAL 8u
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer pixels = ResourceDescriptorHeap[COV_PIXELS];
    RWTexture2D<uint> heads = ResourceDescriptorHeap[COV_HEADS];
    RWStructuredBuffer<CoverageRawFragment> raw = ResourceDescriptorHeap[COV_RAW];
    const uint i = itemOf(gid, lane);
    const bool active = i < min(state.Load(4 * VS_COV_PIXELS), COV_CAP_PIXELS);
    const uint stored = min(state.Load(4 * VS_COV_FRAGMENTS), COV_CAP_FRAGMENTS);
    uint2 p = 0;
    uint head = 0, n = 0;
    if (active)
    {
        p = coverageUnpack(pixels.Load(4 * (COV_PIXEL_LIST + i)));
        head = heads[p];
        for (uint ptr = head; ptr != 0 && ptr <= stored && n < WALK_LIMIT; ++n) ptr = raw[ptr - 1].next;
    }
    const uint keep = min(n, 255u);
    if (n > keep) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_COVERAGE_DEPTH);
    const uint first = waveAppend(state, VS_COV_ALLOC, keep, stored, OVERFLOW_COVERAGE);
    // Written fragments of this pixel: the kept ones without duplicates. A primitive the hardware clips (near plane,
    // guard band) is rasterised in pieces, and conservative rasterisation shades pixels along the cuts once per piece;
    // every such invocation computes the same whole polygon, so the fragments are identical and one is kept. They are
    // adjacent after the sort (same depth and vis id).
    uint unique = 0;
    if (active && first + keep <= stored)  // (always, with consistent lists: the kept counts sum to at most 'stored')
    {
        RWStructuredBuffer<CoverageFragment> sorted = ResourceDescriptorHeap[COV_SORTED];
        if (keep <= SORT_LOCAL)
        {
            CoverageFragment f[SORT_LOCAL];
            uint ptr = head;
            [unroll] for (uint k = 0; k < SORT_LOCAL; ++k)
            {
                f[k] = (CoverageFragment)0;
                f[k].depth = -1;  // below every reversed-Z depth: padding sorts last
                if (k < keep)
                {
                    const CoverageRawFragment r = raw[ptr - 1];
                    f[k] = toSorted(r);
                    ptr = r.next;
                }
            }
            // Optimal 8-input network (19 compare-exchanges, depth 6), nearest first.
#define CX(x, y) if (nearer(f[y], f[x])) { const CoverageFragment t = f[x]; f[x] = f[y]; f[y] = t; }
            CX(0, 2) CX(1, 3) CX(4, 6) CX(5, 7)
            CX(0, 4) CX(1, 5) CX(2, 6) CX(3, 7)
            CX(0, 1) CX(2, 3) CX(4, 5) CX(6, 7)
            CX(2, 4) CX(3, 5)
            CX(1, 4) CX(3, 6)
            CX(1, 2) CX(3, 4) CX(5, 6)
#undef CX
            uint lastVis = 0;
            [unroll] for (uint k2 = 0; k2 < SORT_LOCAL; ++k2)
                if (k2 < keep && (unique == 0 || f[k2].visId != lastVis))
                {
                    sorted[first + unique] = f[k2];
                    lastVis = f[k2].visId;
                    ++unique;
                }
        }
        else
        {
            uint ptr = head;
            for (uint k = 0; k < keep; ++k)
            {
                const CoverageRawFragment r = raw[ptr - 1];
                // Insertion into the written prefix [first, first + k).
                const CoverageFragment x = toSorted(r);
                uint j = k;
                while (j > 0 && nearer(x, sorted[first + j - 1]))
                {
                    sorted[first + j] = sorted[first + j - 1];
                    --j;
                }
                sorted[first + j] = x;
                ptr = r.next;
            }
            uint lastVis = 0;
            for (uint k2 = 0; k2 < keep; ++k2)
            {
                const CoverageFragment x = sorted[first + k2];
                if (unique == 0 || x.visId != lastVis)
                {
                    sorted[first + unique] = x;
                    lastVis = x.visId;
                    ++unique;
                }
            }
        }
    }
    if (active) heads[p] = first | (unique << 24);
    const uint total = WaveActiveSum(unique);
    if (WaveIsFirstLane() && total > 0) pixels.InterlockedAdd(4 * COV_PIXEL_FRAGMENTS, total);
}
#else
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (all(id == 0))
    {
        RWByteAddressBuffer pixels = ResourceDescriptorHeap[COV_PIXELS];
        [unroll] for (uint w = 0; w < COV_PIXEL_LIST; ++w) pixels.Store(4 * w, 0);
    }
    if (id.x >= COV_WIDTH || id.y >= COV_HEIGHT) return;
    RWTexture2D<uint> heads = ResourceDescriptorHeap[COV_HEADS];
    heads[id] = 0;
}
#endif
