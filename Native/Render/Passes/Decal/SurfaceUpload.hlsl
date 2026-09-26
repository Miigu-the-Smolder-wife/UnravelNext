// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Surface state upload (E, A7; SurfaceState.hlsli): scatters the frame's changed records into the persistent copy.
//   MODE 0: brick records, one group per record (117 words: slot, then the 116 words of the brick) -> the pool
//   MODE 1: table entries, one thread per entry (5 words: index, then x, y, z, slot) -> the table
// P[0] = { records SRV (raw), target UAV (raw), count, first record of this dispatch }, P[1].x byte offset of the records,
// P[1].y byte offset of the pool / table in the target (the field buffer: constants, table, pool)
#include "Bindless.hlsli"

#if MODE == 0
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint r = P[0].w + gid.x;
    if (r >= P[0].z || gtid.x >= 116u) return;
    ByteAddressBuffer records = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[0].y];
    const uint base = P[1].x + r * 117u * 4u;
    const uint slot = records.Load(base);
    pool.Store(P[1].y + slot * 464u + gtid.x * 4u, records.Load(base + 4u + gtid.x * 4u));
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint r = P[0].w + id.x;
    if (r >= P[0].z) return;
    ByteAddressBuffer records = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].y];
    const uint base = P[1].x + r * 20u;
    table.Store4(P[1].y + records.Load(base) * 16u, records.Load4(base + 4u));
}
#endif
