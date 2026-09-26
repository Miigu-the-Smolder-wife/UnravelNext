// unx-kernel: cs_6_6 main
// Water records (W, FEATURES_GAME 1.9 stage 1): every coverageSpecial entry of kind 2 (a triangle-stream record, COV_STREAM_ID)
// of a W stream (slots 0..62 with a slot-table row: the water layer's edge records and the coverage layer's fluid
// records) gets the water surface's radiance at its pixel (WaterSurface.hlsli) in
// ViewResources::coverageRecordRadiance[element] (INTERFACES v1.75: f16 exposed linear radiance with aerial
// perspective). M's composite weights it by area x unoccluded share like an opaque record: the refracted light is in it.
// Dispatched indirectly from coverageSpecial's header (64 entries per group).
// P[0] 0, source SRV (band A radiance copy), band A depth SRV, statistics UAV (raw; UNX_NONE)
// P[1] waterVis SRV, waterDepth SRV, slot table SRV, GI cache SRV (UNX_NONE)
// P[2] atmosphere transmittance, multi-scatter, air volume, 0; P[3] VSM page table, blocks, search bound, constants CBV
// P[4] VSM transmittance layers, coverageSpecial SRV (raw), coverageRecords SRV, coverage tile list SRV (raw)
// P[5].x coverageRecordRadiance UAV (raw)
// P[6], P[7] stage 3 (WaterSurface.hlsli waterAppendRays; P[6].x UNX_NONE: none): the round's entries are
// [P[7].x, P[7].x + P[7].y) of coverageSpecial (dispatched directly, rows of groups); without lists the whole list,
// indirectly from its header.
#include "WaterSurface.hlsli"
#include "WaterLinear.hlsli"
#include "Passes/Shading/CoverageSpecial.hlsli"
#include "Passes/Visibility/CoverageLayer.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    ByteAddressBuffer special = ResourceDescriptorHeap[P[4].y];
    const uint local = waterLinear(group, thread, 64), index = P[7].x + local;
    if (local >= P[7].y || index >= special.Load(0)) return;  // (the count is clamped to the list's capacity by V)
    const uint2 entry = special.Load2(4 * (COV_SPECIAL_HEADER + 2 * index));
    if (entry.y != COV_SPECIAL_STREAM) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[4].z];
    const CoverageFragment f = coverageUnpackRecord(records[entry.x]);
    const uint slot = coverageFragmentStreamSlot(f), tri = coverageFragmentStreamTriangle(f);
    if (slot == COV_OCEAN_SLOT || waterSlot(P[1].z, slot).vertices == UNX_NONE) return;  // (the sea: its own pass; not a W stream)
    ByteAddressBuffer list = ResourceDescriptorHeap[P[4].w];
    const uint2 pixel = covRecordPixel(list, entry.x, f);

    WaterShadeSrvs s;
    s.source = P[0].y;
    s.bandADepth = P[0].z;
    s.statistics = P[0].w;
    s.waterVis = P[1].x;
    s.waterDepth = P[1].y;
    s.slots = P[1].z;
    s.giCache = P[1].w;
    s.pad = 0;
    s.atm.transmittance = P[2].x;
    s.atm.multiScatter = P[2].y;
    s.atm.skyView = UNX_NONE;
    s.atm.aerial = P[2].z;
    s.shadow.pageTable = P[3].x;
    s.shadow.pool = UNX_NONE;
    s.shadow.blocks = P[3].y;
    s.shadow.searchBound = P[3].z;
    s.shadow.constants = P[3].w;
    s.shadow.lights = UNX_NONE;
    s.shadow.pad0 = UNX_NONE;
    s.shadow.layers = P[4].x;
    uint stat;
    WaterRayTerms rays;
    const float3 radiance = waterSurfaceShade(s, pixel, slot, tri, stat, rays);
    waterAppendRays(rays, WATER_RAY_RECORD | entry.x, P[0].w);
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[5].x];
    output.Store2(entry.x * 8, covPackRadiance(radiance));
    if (P[0].w != UNX_NONE)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[P[0].w];
        statistics.InterlockedAdd(4 * stat, 1);
    }
}
