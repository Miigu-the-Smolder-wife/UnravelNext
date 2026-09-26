// unx-kernel: cs_6_6 main
// M shading tests (A10 R-1 / R-2): a stand-in for R's FrameServices::traceRefractions that writes a known result per job
// from the job's own fields, so the test checks what TranslucentComposite JOBS=1 wrote and how TranslucentApply weighs it:
//   medium 0xFF (reflection): |d| x 0.05 + 0.003
//   medium 1 (solid glass):   |d| x 0.05 + sigma_a x 1e-3 + ior x 0.01 + bounces x 0.001
// with alpha 1 (traced). P[0] = { jobs SRV (raw), results UAV (raw), max jobs, 0 }.
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint job : SV_DispatchThreadID)
{
    ByteAddressBuffer jobs = ResourceDescriptorHeap[P[0].x];
    if (job >= min(jobs.Load(0), P[0].z)) return;
    const uint at = 16 + job * 48;
    const float3 d = normalize(asfloat(jobs.Load3(at + 16)));
    const uint flags = jobs.Load(at + 28);
    const float3 sigma = asfloat(jobs.Load3(at + 32));
    const float ior = asfloat(jobs.Load(at + 44));
    float3 e = abs(d) * 0.05;
    if ((flags & 0xFFu) == 0xFFu) e += 0.003;
    else e += sigma * 1e-3 + ior * 0.01 + ((flags >> 8) & 3u) * 0.001;
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    results.Store2(job * 8, uint2(f32tof16(e.r) | (f32tof16(e.g) << 16), f32tof16(e.b) | (f32tof16(1.0) << 16)));
}
