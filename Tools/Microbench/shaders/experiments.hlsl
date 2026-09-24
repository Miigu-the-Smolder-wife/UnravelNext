// Kernel variants used to validate the floor kernels (which formulation reaches the hardware peak).
// P[0].x iterations / per-thread count, P[0].y out UAV, P[0].z sentinel, P[0].w mask, P[1].x src SRV.

#ifndef FMA_VARIANT
#define FMA_VARIANT 0
#endif
#ifndef READ_VARIANT
#define READ_VARIANT 0
#endif

[numthreads(256, 1, 1)]
void FmaX(uint tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].y];
    uint iters = P[0].x;
    float s = 0;
#if FMA_VARIANT == 0
    // 8 float4 chains, [loop]
    float4 a0 = float4(tid, tid + 1, tid + 2, tid + 3) * 1e-6, a1 = a0 + 0.1, a2 = a0 + 0.2, a3 = a0 + 0.3, a4 = a0 + 0.4, a5 = a0 + 0.5, a6 = a0 + 0.6, a7 = a0 + 0.7;
    const float4 m = float4(0.99995, 1.00005, 0.99990, 1.00010), c = float4(0.01, -0.02, 0.03, -0.04);
    [loop] for (uint i = 0; i < iters; ++i) { a0 = mad(a0, m, c); a1 = mad(a1, m, c); a2 = mad(a2, m, c); a3 = mad(a3, m, c); a4 = mad(a4, m, c); a5 = mad(a5, m, c); a6 = mad(a6, m, c); a7 = mad(a7, m, c); }
    s = dot(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7, float4(1, 1, 1, 1));
#elif FMA_VARIANT == 1
    // same, unrolled by 8
    float4 a0 = float4(tid, tid + 1, tid + 2, tid + 3) * 1e-6, a1 = a0 + 0.1, a2 = a0 + 0.2, a3 = a0 + 0.3, a4 = a0 + 0.4, a5 = a0 + 0.5, a6 = a0 + 0.6, a7 = a0 + 0.7;
    const float4 m = float4(0.99995, 1.00005, 0.99990, 1.00010), c = float4(0.01, -0.02, 0.03, -0.04);
    [loop] for (uint i = 0; i < iters; i += 8)
    {
        [unroll] for (uint k = 0; k < 8; ++k) { a0 = mad(a0, m, c); a1 = mad(a1, m, c); a2 = mad(a2, m, c); a3 = mad(a3, m, c); a4 = mad(a4, m, c); a5 = mad(a5, m, c); a6 = mad(a6, m, c); a7 = mad(a7, m, c); }
    }
    s = dot(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7, float4(1, 1, 1, 1));
#elif FMA_VARIANT == 2
    // operands vary per chain (no shared constant operands), unrolled by 8
    float4 a0 = float4(tid, tid + 1, tid + 2, tid + 3) * 1e-6, a1 = a0 + 0.1, a2 = a0 + 0.2, a3 = a0 + 0.3, a4 = a0 + 0.4, a5 = a0 + 0.5, a6 = a0 + 0.6, a7 = a0 + 0.7;
    float4 m0 = float4(0.99995, 1.00005, 0.99990, 1.00010) + tid * 1e-9, m1 = m0 * 1.00001, m2 = m0 * 0.99999, m3 = m0 * 1.00002;
    float4 c0 = float4(0.01, -0.02, 0.03, -0.04) + tid * 1e-9, c1 = c0 * 0.5, c2 = c0 * 0.25, c3 = -c0;
    [loop] for (uint i = 0; i < iters; i += 8)
    {
        [unroll] for (uint k = 0; k < 8; ++k) { a0 = mad(a0, m0, c0); a1 = mad(a1, m1, c1); a2 = mad(a2, m2, c2); a3 = mad(a3, m3, c3); a4 = mad(a4, m0, c1); a5 = mad(a5, m1, c2); a6 = mad(a6, m2, c3); a7 = mad(a7, m3, c0); }
    }
    s = dot(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7, float4(1, 1, 1, 1));
#elif FMA_VARIANT == 3
    // 32 scalar chains, explicit a*b+c, unrolled by 4
    float a[32]; [unroll] for (uint j = 0; j < 32; ++j) a[j] = (tid + j) * 1e-6;
    float m = 0.99995 + tid * 1e-9, c = 0.01 + tid * 1e-9;
    [loop] for (uint i = 0; i < iters; i += 4) { [unroll] for (uint k = 0; k < 4; ++k) { [unroll] for (uint j = 0; j < 32; ++j) a[j] = a[j] * m + c; } }
    [unroll] for (uint j2 = 0; j2 < 32; ++j2) s += a[j2];
#elif FMA_VARIANT == 4
    // half-precision packed: 8 chains of half4 (needs -enable-16bit-types)
    half4 a0 = half4(tid & 15, 1, 2, 3) * half(1e-3), a1 = a0 + half(0.1), a2 = a0 + half(0.2), a3 = a0 + half(0.3), a4 = a0 + half(0.4), a5 = a0 + half(0.5), a6 = a0 + half(0.6), a7 = a0 + half(0.7);
    half4 m = half4(0.9995, 1.0005, 0.999, 1.001) + half(tid & 3) * half(1e-4), c = half4(0.01, -0.02, 0.03, -0.04);
    [loop] for (uint i = 0; i < iters; i += 8)
    {
        [unroll] for (uint k = 0; k < 8; ++k) { a0 = mad(a0, m, c); a1 = mad(a1, m, c); a2 = mad(a2, m, c); a3 = mad(a3, m, c); a4 = mad(a4, m, c); a5 = mad(a5, m, c); a6 = mad(a6, m, c); a7 = mad(a7, m, c); }
    }
    s = (float)dot(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7, half4(1, 1, 1, 1));
#elif FMA_VARIANT == 5
    // 16 float4 chains (64 independent scalar FMAs per step), unrolled by 4
    float4 a[16]; [unroll] for (uint j = 0; j < 16; ++j) a[j] = float4(tid, tid + 1, tid + 2, tid + 3) * 1e-6 + j * 0.1;
    float4 m = float4(0.99995, 1.00005, 0.99990, 1.00010) + tid * 1e-9, c = float4(0.01, -0.02, 0.03, -0.04) + tid * 1e-9;
    [loop] for (uint i = 0; i < iters; i += 4) { [unroll] for (uint k = 0; k < 4; ++k) { [unroll] for (uint j = 0; j < 16; ++j) a[j] = mad(a[j], m, c); } }
    float4 t = 0; [unroll] for (uint j2 = 0; j2 < 16; ++j2) t += a[j2]; s = dot(t, float4(1, 1, 1, 1));
#endif
    if (asuint(s) == P[0].z) outb[tid & 1023] = s;
}

[numthreads(256, 1, 1)]
void ReadX(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint per = P[0].x, mask = P[0].w;
    uint4 acc = 0;
#if READ_VARIANT == 0
    // structured uint4, 4 loads in flight, group reads contiguous 256*per*16 B chunk
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[1].x];
    uint base = gid.x * 256u * per;
    uint4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    [loop] for (uint i = 0; i < per; i += 4) { uint e = base + i * 256u + gtid; a0 ^= src[e & mask]; a1 ^= src[(e + 256u) & mask]; a2 ^= src[(e + 512u) & mask]; a3 ^= src[(e + 768u) & mask]; }
    acc = a0 ^ a1 ^ a2 ^ a3;
#elif READ_VARIANT == 1
    // raw Load4, 8 loads in flight
    ByteAddressBuffer src = ResourceDescriptorHeap[P[1].x];
    uint base = gid.x * 256u * per;
    uint4 a[8]; [unroll] for (uint z = 0; z < 8; ++z) a[z] = 0;
    [loop] for (uint i = 0; i < per; i += 8) { [unroll] for (uint k = 0; k < 8; ++k) { uint e = (base + (i + k) * 256u + gtid) & mask; a[k] ^= src.Load4(e * 16u); } }
    [unroll] for (uint k2 = 0; k2 < 8; ++k2) acc ^= a[k2];
#elif READ_VARIANT == 2
    // grid-stride: consecutive groups read consecutive 4 KB, whole dispatch sweeps the buffer per iteration
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[1].x];
    uint groups = P[1].y; uint stride = groups * 256u;
    uint4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    [loop] for (uint i = 0; i < per; i += 4) { uint e = (i * stride) + gid.x * 256u + gtid; a0 ^= src[e & mask]; a1 ^= src[(e + stride) & mask]; a2 ^= src[(e + 2 * stride) & mask]; a3 ^= src[(e + 3 * stride) & mask]; }
    acc = a0 ^ a1 ^ a2 ^ a3;
#elif READ_VARIANT == 3
    // each thread reads 64 B contiguous per step (4 x uint4), warp reads 2 KB contiguous
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[1].x];
    uint base = gid.x * 256u * per;
    uint4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    [loop] for (uint i = 0; i < per; i += 4) { uint e = base + i * 256u + gtid * 4u; a0 ^= src[e & mask]; a1 ^= src[(e + 1) & mask]; a2 ^= src[(e + 2) & mask]; a3 ^= src[(e + 3) & mask]; }
    acc = a0 ^ a1 ^ a2 ^ a3;
#elif READ_VARIANT == 4
    // grid-stride with raw Load4 and 8 in flight
    ByteAddressBuffer src = ResourceDescriptorHeap[P[1].x];
    uint groups = P[1].y; uint stride = groups * 256u;
    uint4 a[8]; [unroll] for (uint z = 0; z < 8; ++z) a[z] = 0;
    [loop] for (uint i = 0; i < per; i += 8) { [unroll] for (uint k = 0; k < 8; ++k) { uint e = ((i + k) * stride + gid.x * 256u + gtid) & mask; a[k] ^= src.Load4(e * 16u); } }
    [unroll] for (uint k2 = 0; k2 < 8; ++k2) acc ^= a[k2];
#elif READ_VARIANT == 5
    // typed Buffer<uint4> (R32G32B32A32_UINT view) grid-stride, 4 in flight
    Buffer<uint4> src = ResourceDescriptorHeap[P[1].z];
    uint groups = P[1].y; uint stride = groups * 256u;
    uint4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    [loop] for (uint i = 0; i < per; i += 4) { uint e = (i * stride) + gid.x * 256u + gtid; a0 ^= src[e & mask]; a1 ^= src[(e + stride) & mask]; a2 ^= src[(e + 2 * stride) & mask]; a3 ^= src[(e + 3 * stride) & mask]; }
    acc = a0 ^ a1 ^ a2 ^ a3;
#endif
    if ((acc.x ^ acc.y ^ acc.z ^ acc.w) == P[0].z) outb[gtid] = acc.x;
}
