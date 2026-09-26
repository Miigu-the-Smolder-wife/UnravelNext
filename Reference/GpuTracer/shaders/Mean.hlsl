// unx-kernel: cs_6_6 main
// The accumulation so far as an image on the GPU (photo mode's progressive display; GpuPathTracer::currentImageResource):
// per pixel, each half's double sum x its 1 / samples (double, from the CPU) rounded to float, x exposure, then the mean
// of the halves - the operations and their order of GpuPathTracer::current(), so the image equals the read-back one.
// Also the halves' relMSE terms (metrics::relMse(halfA, halfB): per channel (b - a)^2 / (a^2 + 0.01) in double on the
// float halves), summed per 8x8 group in a fixed tree into one double per group; MeanSum.hlsl adds the groups.
// Root: constants, x0 = output UAV (RWTexture2D<float4>), y0 = width, w = height, h / sampleBegin = 1 / samples of half A
// (double bits lo / hi), sampleEnd / pathBase = the same for half B, pathCount = exposure (float bits), passIndex = the
// group sums' UAV (RWByteAddressBuffer, one double per group, row-major over the dispatch's groups).
#include "Common.hlsli"

groupshared double gs_sum[64];

// 1 / d in double with base double operations only (no double division, an optional D3D12 capability): the float
// reciprocal refined by two Newton steps (relative error 1e-7 -> 1e-14 -> below double rounding). d >= 0.01 here.
double rcpDouble(double d)
{
    double q = (double)(1.0f / (float)d);
    q = q * (2.0 - d * q);
    q = q * (2.0 - d * q);
    return q;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint flat : SV_GroupIndex)
{
    const uint W = g_root.y0, H = g_root.w;
    double term = 0;
    if (id.x < W && id.y < H)
    {
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
        [unroll] for (uint c = 0; c < 3; ++c)
        {
            const double r = a[c], t = b[c];
            term += (t - r) * (t - r) * rcpDouble(r * r + 0.01);
        }
    }
    gs_sum[flat] = term;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1)
    {
        if (flat < s) gs_sum[flat] += gs_sum[flat + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (flat == 0)
    {
        RWByteAddressBuffer sums = ResourceDescriptorHeap[g_root.passIndex];
        uint lo, hi;
        asuint(gs_sum[0], lo, hi);
        sums.Store2((group.y * ((W + 7) / 8) + group.x) * 8, uint2(lo, hi));
    }
}
