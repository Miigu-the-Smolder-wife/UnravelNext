// unx-kernel: cs_6_6 main
// The accumulation so far as an image on the GPU (photo mode's progressive display; GpuPathTracer::currentImageResource):
// per pixel, each half's double sum x its 1 / samples (double, from the CPU) rounded to float, x exposure, then the mean
// of the halves - the operations and their order of GpuPathTracer::current(), so the image equals the read-back one.
// Root: constants, x0 = output UAV (RWTexture2D<float4>), y0 = width, w = height, h / sampleBegin = 1 / samples of half A
// (double bits lo / hi), sampleEnd / pathBase = the same for half B, pathCount = exposure (float bits).
#include "Common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint W = g_root.y0, H = g_root.w;
    if (id.x >= W || id.y >= H) return;
    const RtConstants C = rtC();
    const double inv0 = asdouble(g_root.h, g_root.sampleBegin), inv1 = asdouble(g_root.sampleEnd, g_root.pathBase);
    const float exposure = asfloat(g_root.pathCount);
    const uint addr = (id.y * W + id.x) * 24;
    RWByteAddressBuffer a0 = ResourceDescriptorHeap[C.b.accum0];  // the accumulators have UAV descriptors only
    RWByteAddressBuffer a1 = ResourceDescriptorHeap[C.b.accum1];
    const uint4 p0 = a0.Load4(addr), q0 = a1.Load4(addr);
    const uint2 p1 = a0.Load2(addr + 16), q1 = a1.Load2(addr + 16);
    const double3 s0 = double3(asdouble(p0.x, p0.y), asdouble(p0.z, p0.w), asdouble(p1.x, p1.y));
    const double3 s1 = double3(asdouble(q0.x, q0.y), asdouble(q0.z, q0.w), asdouble(q1.x, q1.y));
    const float3 a = float3((float)(s0.x * inv0), (float)(s0.y * inv0), (float)(s0.z * inv0)) * exposure;
    const float3 b = float3((float)(s1.x * inv1), (float)(s1.y * inv1), (float)(s1.z * inv1)) * exposure;
    RWTexture2D<float4> image = ResourceDescriptorHeap[g_root.x0];
    image[id.xy] = float4(0.5f * (a + b), 1);
}
