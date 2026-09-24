// unx-kernel: cs_6_6 main
// Render-graph test kernel: one thread group that reads one element of every input and writes one element of every
// output, so the graph's barriers, layouts and aliasing are exercised with real accesses but negligible work.
// P[0].x inputs (0-3), P[0].y outputs (0-2), P[0].z seed
// P[1].xyz input descriptor indices, P[2].xyz input kinds, P[3].xy output indices, P[3].zw output kinds
#include "Bindless.hlsli"

#define KIND_FLOAT 0
#define KIND_UINT 1
#define KIND_BUFFER 2
#define KIND_UNORM 3
#define KIND_FLOAT3D 4

float4 loadAny(uint index, uint kind, uint2 p)
{
    if (kind == KIND_FLOAT || kind == KIND_UNORM)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[index];
        return t.Load(int3(p, 0));
    }
    if (kind == KIND_UINT)
    {
        Texture2D<uint4> t = ResourceDescriptorHeap[index];
        return float4(t.Load(int3(p, 0)));
    }
    if (kind == KIND_BUFFER)
    {
        ByteAddressBuffer b = ResourceDescriptorHeap[index];
        return asfloat(b.Load4(p.x * 16));
    }
    Texture3D<float4> v = ResourceDescriptorHeap[index];
    return v.Load(int4(p, 0, 0));
}

void storeAny(uint index, uint kind, uint2 p, float4 value)
{
    if (kind == KIND_FLOAT)
    {
        RWTexture2D<float4> t = ResourceDescriptorHeap[index];
        t[p] = value;
    }
    else if (kind == KIND_UINT)
    {
        RWTexture2D<uint4> t = ResourceDescriptorHeap[index];
        t[p] = asuint(value);
    }
    else if (kind == KIND_BUFFER)
    {
        RWByteAddressBuffer b = ResourceDescriptorHeap[index];
        b.Store4(p.x * 16, asuint(value));
    }
    else if (kind == KIND_UNORM)
    {
        RWTexture2D<unorm float4> t = ResourceDescriptorHeap[index];
        t[p] = saturate(value);
    }
    else
    {
        RWTexture3D<float4> v = ResourceDescriptorHeap[index];
        v[uint3(p, 0)] = value;
    }
}

[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    const uint2 p = uint2(tid, 0);
    float4 acc = float4(P[0].z, tid, 0, 1);
    [unroll] for (uint i = 0; i < 3; ++i)
        if (i < P[0].x) acc += loadAny(P[1][i], P[2][i], p);
    if (P[0].y > 0) storeAny(P[3].x, P[3].z, p, acc);
    if (P[0].y > 1) storeAny(P[3].y, P[3].w, p, acc * 0.5);
}
