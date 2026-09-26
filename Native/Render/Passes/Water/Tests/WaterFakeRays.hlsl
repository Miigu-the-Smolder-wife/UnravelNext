// unx-kernel: cs_6_6 main
// Water stage 3 test: a stand-in for R's FrameServices::traceRefractions that writes a known result per job from the
// job's own fields into its output slot, so WaterSurfaceTests checks the jobs W wrote (direction, medium, absorption,
// index, bounces) and how the apply pass weighs them:
//   medium 0xFF (reflection): |d| x 20 + 1 (large: the test measures the camera's air path from it, above band A's fp16)
//   medium 0 (water):         |d| x 0.2 + sigma_a x 0.01 + ior x 0.02 + bounces x 0.003
// with alpha 1 (traced). P[0].w selects what is written: 0 both, 1 zero radiance (the value without the replaced terms),
// 2 reflection jobs only (refraction jobs zero), 3 refraction jobs only (reflection jobs zero).
// P[0] = { jobs SRV (raw), results UAV (raw), max jobs, mode }
#include "Bindless.hlsli"
#include "../WaterLinear.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint job = waterLinear(group, thread, 64);
    ByteAddressBuffer jobs = ResourceDescriptorHeap[P[0].x];
    if (job >= min(jobs.Load(0), P[0].z)) return;
    const uint at = 16 + job * 48;
    const uint slot = jobs.Load(at + 12);
    const float3 d = asfloat(jobs.Load3(at + 16));
    const uint flags = jobs.Load(at + 28);
    const float3 sigma = asfloat(jobs.Load3(at + 32));
    const float ior = asfloat(jobs.Load(at + 44));
    float3 e = abs(d) * 0.2;
    if ((flags & 0xFFu) == 0xFFu) e = abs(d) * 20 + 1;
    else e += sigma * 0.01 + ior * 0.02 + ((flags >> 8) & 3u) * 0.003 + (flags & 0xFFu) * 100.0;  // (medium != 0: visible)
    const bool reflection = (flags & 0xFFu) == 0xFFu;
    if (P[0].w == 1 || (P[0].w == 2 && !reflection) || (P[0].w == 3 && reflection)) e = 0;
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    results.Store2(slot * 8, uint2(f32tof16(e.r) | (f32tof16(e.g) << 16), f32tof16(e.b) | (f32tof16(1.0) << 16)));
}
