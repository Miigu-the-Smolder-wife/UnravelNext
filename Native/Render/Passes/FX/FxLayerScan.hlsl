// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Particle render pass, tile lists:
//   STEP=0 clear: the tile counts and fills, the pass counters and the edge block count (thread per tile).
//   STEP=1 scan: one group, the exclusive prefix of the tile counts -> tile starts, the entry total (FX_LAYER_COUNTER_ENTRIES;
//          more than the entry buffer holds sets FX_LAYER_STATUS_ENTRY_OVERFLOW). Group-memory scan only (no wave operations).
#include "Passes/FX/ParticleLayerPass.hlsli"

groupshared uint gs_partial[1024];

[numthreads(1024, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    const LayerConstants c = fxLayerConstants();
    const uint tiles = c.tilesX * c.tilesY;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
#if STEP == 0
    if (id.x < tiles)
    {
        RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[c.tileFill];
        counts[id.x] = 0u;
        fill[id.x] = 0u;
    }
    if (id.x < 4u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
        counters[id.x] = 0u;
    }
    if (id.x == 0u)
    {
        RWByteAddressBuffer edges = ResourceDescriptorHeap[c.edgeBlocks];
        edges.Store(0u, 0u);  // edge block count
    }
#else
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[c.tileStarts];
    const uint t = gtid.x, per = (tiles + 1023u) / 1024u;
    uint local = 0u;
    for (uint k = 0u; k < per; ++k)
    {
        const uint at = t * per + k;
        if (at < tiles) local += counts[at];
    }
    gs_partial[t] = local;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = gs_partial[t] - local;
    for (uint k2 = 0u; k2 < per; ++k2)
    {
        const uint at = t * per + k2;
        if (at < tiles)
        {
            starts[at] = run;
            run += counts[at];
        }
    }
    if (t == 1023u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
        const uint total = gs_partial[1023];
        counters[FX_LAYER_COUNTER_ENTRIES] = total;
        if (total > c.entryCapacity) InterlockedOr(counters[FX_LAYER_COUNTER_STATUS], FX_LAYER_STATUS_ENTRY_OVERFLOW);
    }
#endif
}
