// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// A compacted ray list's bookkeeping. A list (LgCompactTraces.hlsl: the screen probes' rays; ReflectionCompactTraces.hlsl:
// the reflection jobs; MegaLightsCompact.hlsl: the light samples - the reference's CompactTraces) holds the threads a ray
// pass has work for, so that its DispatchRays launches those alone: a raw buffer, word 0 = the count, entries of 4 B from
// byte 16 on. Filled by one atomic a wave, in no order a reader may count on.
//   MODE 0  one thread: the count = 0 (before the kernel that fills the list).
//   MODE 1  one thread: the count, held to the capacity, back into word 0 and into the Width of the pass's indirect
//           DispatchRays descriptions. Chunk c's descriptions (P[1].w of them side by side: a pipeline's variants) take
//           the entries [c x P[1].y, (c + 1) x P[1].y): Width = its entries, Height = Depth = 1; a chunk without entries
//           0, 0, 0 (a dispatch that launches nothing). A dispatch launches at most P[1].y threads, whatever the frame's
//           count is; the chunks are a count of the CPU's (the capacity over P[1].y).
// P[0] = { list UAV (raw), capacity (entries), descriptions UAV (raw), byte offset of chunk 0's first description's Width }
// P[1] = { bytes from a chunk's descriptions to the next chunk's, entries a chunk, chunks, descriptions a chunk }
// P[2].x = the description stride (bytes between a chunk's descriptions)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
#if MODE == 0
    list.Store4(0, uint4(0, 0, 0, 0));
#else
    const uint count = min(list.Load(0), P[0].y);
    list.Store(0, count);
    RWByteAddressBuffer descriptions = ResourceDescriptorHeap[P[0].z];
    const uint perChunk = max(P[1].y, 1u);
    for (uint c = 0; c < P[1].z; ++c)
    {
        const uint entries = count > c * perChunk ? min(count - c * perChunk, perChunk) : 0u;
        for (uint d = 0; d < P[1].w; ++d)
            descriptions.Store3(P[0].w + c * P[1].x + d * P[2].x, entries > 0 ? uint3(entries, 1, 1) : uint3(0, 0, 0));  // Width, Height, Depth
    }
#endif
}
