// Compute floor kernels: FMA throughput, DRAM/L2 bandwidth, random gather, pointer-chase latency,
// texture sampling rate, atomics. Every kernel keeps its result live through a never-true sentinel
// compare so nothing is dead-code eliminated and no extra memory traffic is added.

// ---------------------------------------------------------------- FMA throughput
// P[0].x iterations, P[0].y out UAV index, P[0].z sentinel.
// FLOPs per thread = iterations * 8 chains * 4 lanes * 2.
[numthreads(256, 1, 1)]
void FmaCS(uint tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].y];
    uint iters = P[0].x; // multiple of 8
    float4 a0 = float4(tid, tid + 1, tid + 2, tid + 3) * 1e-6;
    float4 a1 = a0 + 0.1, a2 = a0 + 0.2, a3 = a0 + 0.3, a4 = a0 + 0.4, a5 = a0 + 0.5, a6 = a0 + 0.6, a7 = a0 + 0.7;
    const float4 m = float4(0.99995, 1.00005, 0.99990, 1.00010);
    const float4 c = float4(0.01, -0.02, 0.03, -0.04);
    [loop] for (uint i = 0; i < iters; i += 8)
    {
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            a0 = mad(a0, m, c); a1 = mad(a1, m, c); a2 = mad(a2, m, c); a3 = mad(a3, m, c);
            a4 = mad(a4, m, c); a5 = mad(a5, m, c); a6 = mad(a6, m, c); a7 = mad(a7, m, c);
        }
    }
    float s = dot(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7, float4(1, 1, 1, 1));
    if (asuint(s) == P[0].z) outb[tid & 1023] = s;
}

// ---------------------------------------------------------------- sequential read (DRAM or L2 via mask)
// P[0].x src SRV, P[0].y out UAV, P[0].z uint4 per thread (multiple of 4), P[0].w element mask (pow2-1), P[1].x sentinel, P[1].y group count.
[numthreads(256, 1, 1)]
void ReadCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint per = P[0].z, mask = P[0].w, stride = P[1].y * 256u; // grid-stride: the whole dispatch sweeps contiguously
    uint4 acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0;
    [loop] for (uint i = 0; i < per; i += 4)
    {
        uint e = i * stride + gid.x * 256u + gtid;
        acc0 ^= src[(e) & mask];
        acc1 ^= src[(e + stride) & mask];
        acc2 ^= src[(e + 2u * stride) & mask];
        acc3 ^= src[(e + 3u * stride) & mask];
    }
    uint4 acc = acc0 ^ acc1 ^ acc2 ^ acc3;
    if ((acc.x ^ acc.y ^ acc.z ^ acc.w) == P[1].x) outb[gtid] = acc.x;
}


// ---------------------------------------------------------------- random 4 KB chunk reads (L2 vs DRAM by window)
// P[0].x src SRV, P[0].y out UAV, P[0].z chunks per thread (multiple of 4), P[0].w window element mask (pow2-1, >= 4 KB),
// P[1].x sentinel, P[1].y seed. Each group reads a random contiguous 4 KB chunk; reuse distance defeats L1.
[numthreads(256, 1, 1)]
void ReadChunkCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint per = P[0].z, chunkMask = P[0].w >> 8;
    uint4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    [loop] for (uint i = 0; i < per; i += 4)
    {
        uint h0 = pcg(gid.x * 65599u + i * 977u + P[1].y), h1 = pcg(h0), h2 = pcg(h1), h3 = pcg(h2);
        a0 ^= src[((h0 & chunkMask) << 8) + gtid];
        a1 ^= src[((h1 & chunkMask) << 8) + gtid];
        a2 ^= src[((h2 & chunkMask) << 8) + gtid];
        a3 ^= src[((h3 & chunkMask) << 8) + gtid];
    }
    uint4 acc = a0 ^ a1 ^ a2 ^ a3;
    if ((acc.x ^ acc.y ^ acc.z ^ acc.w) == P[1].x) outb[gtid] = acc.x;
}

// ---------------------------------------------------------------- sequential write
// P[0].x dst UAV, P[0].z uint4 per thread, P[0].w mask, P[1].y content (1 = hashed), P[1].z group count.
[numthreads(256, 1, 1)]
void WriteCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    RWStructuredBuffer<uint4> dst = ResourceDescriptorHeap[P[0].x];
    uint per = P[0].z, mask = P[0].w, stride = P[1].z * 256u;
    [loop] for (uint i = 0; i < per; ++i)
    {
        uint e = i * stride + gid.x * 256u + gtid;
        if (P[1].y != 0) { uint h = pcg(e ^ P[1].x); dst[e & mask] = uint4(h, pcg(h), pcg(h + 1u), pcg(h + 2u)); }
        else dst[e & mask] = uint4(e, gtid, gid.x, P[1].x);
    }
}

// ---------------------------------------------------------------- copy (read + write)
// P[0].x src SRV, P[0].y dst UAV, P[0].z uint4 per thread, P[0].w mask, P[1].z group count.
[numthreads(256, 1, 1)]
void CopyCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    StructuredBuffer<uint4> src = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint4> dst = ResourceDescriptorHeap[P[0].y];
    uint per = P[0].z, mask = P[0].w, stride = P[1].z * 256u;
    [loop] for (uint i = 0; i < per; ++i)
    {
        uint e = (i * stride + gid.x * 256u + gtid) & mask;
        dst[e] = src[e];
    }
}

// ---------------------------------------------------------------- random gather
// P[0].x src raw SRV, P[0].y out UAV, P[0].z loads per thread (multiple of 4), P[0].w byte mask (pow2-1),
// P[1].x sentinel, P[1].y seed, P[1].z granularity: 0 = 4 B loads, 1 = 64 B (4 x Load4 within one 64 B line).
[numthreads(256, 1, 1)]
void GatherCS(uint tid : SV_DispatchThreadID)
{
    ByteAddressBuffer src = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint n = P[0].z, mask = P[0].w;
    uint s = tid * 7919u + P[1].y;
    uint acc = 0;
    if (P[1].z == 0)
    {
        [loop] for (uint i = 0; i < n; i += 4)
        {
            uint s0 = pcg(s), s1 = pcg(s0), s2 = pcg(s1), s3 = pcg(s2);
            s = s3;
            acc ^= src.Load((s0 & mask) & ~3u);
            acc ^= src.Load((s1 & mask) & ~3u);
            acc ^= src.Load((s2 & mask) & ~3u);
            acc ^= src.Load((s3 & mask) & ~3u);
        }
    }
    else
    {
        [loop] for (uint i = 0; i < n; ++i)
        {
            s = pcg(s);
            uint a = (s & mask) & ~63u;
            uint4 v0 = src.Load4(a), v1 = src.Load4(a + 16), v2 = src.Load4(a + 32), v3 = src.Load4(a + 48);
            acc ^= v0.x ^ v1.y ^ v2.z ^ v3.w;
        }
    }
    if (acc == P[1].x) outb[tid & 1023] = acc;
}

// ---------------------------------------------------------------- pointer chase (single thread, dependent loads)
// P[0].x perm SRV, P[0].y out UAV, P[0].z chain length, P[1].y start index.
[numthreads(1, 1, 1)]
void ChaseCS(uint tid : SV_DispatchThreadID)
{
    StructuredBuffer<uint> perm = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint n = P[0].z;
    uint idx = P[1].y;
    [loop] for (uint i = 0; i < n; ++i) idx = perm[idx];
    outb[0] = idx;
}

// ---------------------------------------------------------------- texture sampling
// P[0].x texture SRV, P[0].y out UAV, P[0].z samples per thread, P[0].w mode
//   0: coherent 8x8 block, bilinear level 0     1: incoherent random uv, bilinear level 0
//   2: coherent 8x8 block, trilinear SampleGrad  3: coherent, point level 0
// P[1].x sentinel, P[1].y seed, P[1].z texture size in texels.
[numthreads(64, 1, 1)]
void TexCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    Texture2D<float4> t = ResourceDescriptorHeap[P[0].x];
    SamplerState lin = SamplerDescriptorHeap[0];
    SamplerState pnt = SamplerDescriptorHeap[1];
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].y];
    uint n = P[0].z, mode = P[0].w;
    float texel = 1.0 / (float)P[1].z;
    uint seed = pcg((gid.x + gid.y * 65536u) * 131u + P[1].y);
    float2 base = float2(u01(seed), u01(seed));
    float2 uv = base + float2(gtid & 7, gtid >> 3) * texel;
    float2 grad = float2(texel * 1.5, 0);
    float4 acc = 0;
    [loop] for (uint i = 0; i < n; ++i)
    {
        float2 o = uv + float2(i, i * 0.5) * texel;
        if (mode == 1) { uint s2 = pcg(seed + i * 977u + gtid); o = float2(u01(s2), u01(s2)); }
        if (mode == 2) acc += t.SampleGrad(lin, o, grad, grad.yx);
        else if (mode == 3) acc += t.SampleLevel(pnt, o, 0);
        else acc += t.SampleLevel(lin, o, 0);
    }
    float s = dot(acc, float4(1, 1, 1, 1));
    if (asuint(s) == P[1].x) outb[gtid] = s;
}

// ---------------------------------------------------------------- atomics
// P[0].x buffer UAV, P[0].z atomics per thread, P[0].w address mask (0 = single address), P[1].y seed.
[numthreads(256, 1, 1)]
void AtomicCS(uint tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> buf = ResourceDescriptorHeap[P[0].x];
    uint n = P[0].z, mask = P[0].w;
    uint s = tid * 2654435761u + P[1].y;
    [loop] for (uint i = 0; i < n; ++i)
    {
        s = pcg(s);
        InterlockedAdd(buf[s & mask], 1u);
    }
}

// ---------------------------------------------------------------- tiny kernel for dispatch/barrier overhead
[numthreads(64, 1, 1)]
void TinyCS(uint tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> buf = ResourceDescriptorHeap[P[0].x];
#ifdef VARIANT
    buf[tid] = buf[tid] * 3u + VARIANT;
#else
    buf[tid] = buf[tid] + 1u;
#endif
}
