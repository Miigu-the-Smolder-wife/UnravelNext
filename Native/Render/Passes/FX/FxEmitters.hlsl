// unx-kernel: cs_6_6 main
// Emitter table of the tick (NativeVfxStream.h, NV_STREAM_EMITTER_DELTA): the persistent GPU table receives the blocks
// the packet sends (thread per block: table[rows[k]] = block k) and the row patches (thread per patch: the per-tick
// fields flags, next/death/dying birth, death_event, output_base, parent_event, parent_row and rebase of the held row;
// NV_StreamEmitterPatch). Blocks and patches cover disjoint rows. Both stamp their row with the packet serial; FxBegin
// then clears the per-tick fields of the rows that got neither (the stream reads them as absent). A whole-table packet
// sends every row as a block (rows 0..n-1).
#include "Passes/FX/Particles.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    FX_RWBUFFER(StreamEmitter, table, g_emitters);
    FX_RWBUFFER(uint, stamp, g_emitterStamp);
    if (id.x < g_updateCount)
    {
        FX_BUFFER(StreamEmitter, updates, g_emitterUpdates);
        FX_BUFFER(uint, rows, g_emitterUpdateRows);
        const uint row = rows[id.x];
        if (row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
        table[row] = updates[id.x];
        stamp[row] = g_serial;
        return;
    }
    const uint k = id.x - g_updateCount;
    if (k >= g_patchCount) return;
    FX_BUFFER(StreamEmitterPatch, patches, g_emitterPatches);
    const StreamEmitterPatch p = patches[k];
    if (p.row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
    StreamEmitter e = table[p.row];
    e.flags = p.flags;
    e.nextBirth = p.nextBirth; e.deathBirth = p.deathBirth; e.dyingBirth = p.dyingBirth; e.deathEvent = p.deathEvent;
    e.outputBase = p.outputBase; e.parentEvent = p.parentEvent; e.parentRow = p.parentRow;
    e.rebase = p.rebase;
    table[p.row] = e;
    stamp[p.row] = g_serial;
}
