// unx-kernel: cs_6_6 main
// A10 glass, R-1 / R-2: after TranslucentComposite JOBS=1 appended a band's jobs and records, the dispatch arguments:
// the jobs header's { count, count, 1, 1 } (FrameServices::traceRefractions dispatches its rays from it) and the records
// header's { count, groups of 64, 1, 1 } (TranslucentApply, ExecuteIndirect at byte 4). Then the next band starts from
// zero counts (ExposureClear on both headers). P[0] = { jobs UAV (raw), records UAV (raw), max jobs, max records }.
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer jobs = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer records = ResourceDescriptorHeap[P[0].y];
    const uint j = min(jobs.Load(0), P[0].z), r = min(records.Load(0), P[0].w);
    jobs.Store4(0, uint4(j, j, 1, 1));
    records.Store4(0, uint4(r, (r + 63) / 64, 1, 1));
}
