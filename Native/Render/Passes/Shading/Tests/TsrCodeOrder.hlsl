// unx-kernel: cs_6_6 main
// Exercise the production conversion functions with every per-channel code.
#define main unusedTsrReject
#include "Passes/Shading/TsrReject.hlsl"
#undef main

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= 2048) return;
    const uint3 code = uint3(i, i, i >> 1);
    const uint packed = packCodes(code);
    const float3 decoded = unpack(packed);
    uint errors = pack(decoded) != packed ? 1u : 0u;
    if (i > 0)
    {
        const uint previous = i - 1;
        const float3 before = unpack(packCodes(uint3(previous, previous, previous >> 1)));
        if (!all(decoded.xy > before.xy)) errors |= 2u;
        if ((i & 1u) == 0 && !(decoded.z > before.z)) errors |= 4u;
    }
    RWStructuredBuffer<uint> output = ResourceDescriptorHeap[P[0].x];
    output[i] = errors;
}
