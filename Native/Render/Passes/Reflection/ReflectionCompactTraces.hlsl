// unx-kernel: cs_6_6 main
// r.refl.lumen.compact (reflection.lumen_compact_traces; the reference's CompactTraces before its hardware ray tracing):
// the jobs that still need a world ray after the screen traces, as a list (RayTracing/CompactDispatch.hlsl), so that
// r.refl.lumen.trace launches a thread per ray it traces. Over the whole job list its threads of the jobs the screen
// trace finished return at once and leave their waves partly idle.
// Thread = pixel (a group: one 8 x 8 tile; a tile without traced pixels is left at its first load). A traced pixel's job
// whose entry in the job list is not marked done (ReflectionScreenTrace.hlsl: bit 31) is appended: the job's index. One
// atomic a wave.
// P[0] = { modes SRV, jobs SRV, reflection SRV (the tiles' validity rows from row P[1].z on), list UAV (raw: word 0 the
//          count, entries from byte 16) }, P[1] = { width, height, pixel rows H, capacity (entries) }
#include "Passes/Reflection/ReflectionInternal.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    const uint2 size = P[1].xy;
    const uint2 pixel = tile * 8 + local;
    Texture2D<float4> reflection = ResourceDescriptorHeap[P[0].z];
    if (reflection.Load(int3(tile.x, P[1].z + tile.y, 0)).a < 0.5) return;  // a tile without traced or planar pixels (the whole group)
    bool need = false;
    uint job = REFL_NO_JOB;
    if (all(pixel < size))
    {
        Texture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
        const uint m = modes.Load(int3(pixel, 0));
        job = reflJob(m);
        if (reflMode(m) == REFL_M && job != REFL_NO_JOB)
        {
            StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].y];
            need = (jobs[job] & REFL_JOB_DONE) == 0;
        }
    }
    const uint count = WaveActiveCountBits(need);
    uint base = 0;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].w];
    if (WaveIsFirstLane() && count > 0) list.InterlockedAdd(0, count, base);
    base = WaveReadLaneFirst(base);
    const uint slot = base + WavePrefixCountBits(need);
    if (need && slot < P[1].w) list.Store(16 + 4 * slot, job);
}
