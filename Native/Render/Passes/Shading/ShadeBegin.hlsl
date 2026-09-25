// unx-kernel: cs_6_6 main
// Start of the shading passes: resets the edge args (composite dispatch (0, 1, 1), pixel count 0; Edge.hlsli) and bounds
// every (class, band) tile count in the resolve's dispatch arguments by that band's tile count (MaterialSystem.h
// ResolveOutputs). A count cannot exceed it by construction; the bound keeps an indirect dispatch finite if the arguments
// were ever clobbered (e.g. by a transient alias written late) instead of launching billions of groups and hanging the
// GPU. The bounded counts per class are summed after the arguments (statistics).
// It also turns S's shadow overflow fallback list (INTERFACES 7.3: word 0 = tile count) into M's own dispatch arguments for
// the fallback shading kernel, bounded the same way (the list is that kernel's SRV, which the graph cannot also bind as
// its indirect arguments).
// P[0] = { edge args UAV (raw, 24 B), class tile args UAV (raw), classes, screen bands }
// P[1] = { S's fallback tile list SRV (raw) or UNX_NONE, M's fallback args UAV (raw, 12 B) or UNX_NONE, tilesX, view height }
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer edge = ResourceDescriptorHeap[P[0].x];
    edge.Store3(0, uint3(0, 1, 1));
    edge.Store3(12, uint3(0, 1, 1));
    RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].y];
    const uint classes = P[0].z, bands = P[0].w, tilesX = P[1].z, height = P[1].w;
    const uint tilesY = (height + 7) / 8;
    for (uint c = 0; c < classes; ++c)
    {
        uint total = 0;
        for (uint b = 0; b < bands; ++b)
        {
            const uint first = min(height, (height * b / bands) & ~7u) / 8;
            const uint last = b + 1 >= bands ? tilesY : min(height, (height * (b + 1) / bands) & ~7u) / 8;
            const uint at = 12 * (c * bands + b);
            const uint n = min(tiles.Load(at), tilesX * (last - first));
            tiles.Store(at, n);
            total += n;
        }
        tiles.Store(12 * classes * bands + 4 * c, total);
    }
    if (P[1].y != UNX_NONE)
    {
        uint n = 0;
        if (P[1].x != UNX_NONE)
        {
            ByteAddressBuffer fallback = ResourceDescriptorHeap[P[1].x];
            n = min(fallback.Load(0), tilesX * tilesY);
        }
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].y];
        args.Store3(0, uint3(n, 1, 1));
    }
}
