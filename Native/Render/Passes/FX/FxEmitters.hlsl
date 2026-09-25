// unx-kernel: cs_6_6 main
// Emitter table of the tick (NativeVfxStream.h, NV_STREAM_EMITTER_DELTA): the persistent GPU table receives the blocks
// the packet sends (thread per block: table[rows[k]] = block k, stamp[row] = the packet serial). A whole-table packet
// sends every row (rows 0..n-1). FxBegin then clears the per-tick fields of the rows not sent (rebase, TRANSPORT,
// SOURCE, KILLED, parent event), which the stream defines as absent.
#include "Passes/FX/Particles.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_updateCount) return;
    FX_BUFFER(StreamEmitter, updates, g_emitterUpdates);
    FX_BUFFER(uint, rows, g_emitterUpdateRows);
    FX_RWBUFFER(StreamEmitter, table, g_emitters);
    FX_RWBUFFER(uint, stamp, g_emitterStamp);
    const uint row = rows[id.x];
    if (row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
    table[row] = updates[id.x];
    stamp[row] = g_serial;
}
