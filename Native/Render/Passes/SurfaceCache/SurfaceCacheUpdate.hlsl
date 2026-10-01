// unx-kernel: cs_6_6 main
// The surface cache's upkeep (SurfaceCache.hlsli), after SurfaceCacheBegin, one thread per slot of the cells (P[0].z = 0)
// or of the probes (P[0].z = 1). An entry marked since the last upkeep is in use; the others age. An entry is freed when
// it has not been marked for more than the header's max unused frames. An entry whose point no longer maps to its key
// (the camera moved it to another level) is not marked any more - the new level's entry is - and ages out; until then
// reads fall back on it (scRead). The live entries are listed for the lighting passes: those already lit from the list's
// start, those not lit yet from its end (they are lit first).
// P[0] = { cache UAV, cells N, table (0 cells, 1 probes), 0 }.
#include "Passes/SurfaceCache/SurfaceCache.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const uint n = P[0].y;
    const bool probes = P[0].z != 0;
    const uint count = probes ? scProbeCount(n) : n;
    const uint slot = id.x;
    if (slot >= count) return;
    const uint keyOffset = probes ? scProbeKeysOffset(n, slot) : scKeysOffset(slot);
    const uint key = b.Load(keyOffset);
    if (key == 0u) return;
    const uint headOffset = probes ? scProbeHeadsOffset(n, slot) : scHeadsOffset(n, slot);
    const uint head = b.Load(headOffset);
    uint unused = (head >> 16) & 0xFFu;
    const uint lit = (head >> 8) & 0xFFu;
    if (head & SC_HEAD_MARKED) unused = 0;
    else unused = min(unused + 1, 255u);
    const bool keep = (head & (SC_HEAD_MARKED | SC_HEAD_VALID)) != 0 && unused <= b.Load(28);
    if (!keep)
    {
        b.Store(headOffset, 0u);
        b.Store(keyOffset, 0u);
        return;
    }
    b.Store(headOffset, SC_HEAD_VALID | (lit << 8) | (unused << 16) | (head & 0xFF000000u));  // (bits 24-31: the stochastic direct light's frames)
    uint index;
    b.InterlockedAdd((probes ? 32u : 8u) + (lit != 0 ? 0u : 4u), 1u, index);
    const uint at = lit != 0 ? index : count - 1 - index;
    b.Store(probes ? scProbeListOffset(n, at) : scListOffset(n, at), slot);
}
