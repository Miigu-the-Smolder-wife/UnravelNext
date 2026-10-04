// unx-kernel: cs_6_6 main
#define main codeInputs
#include "Passes/Shading/Tests/TsrCodeInput.hlsl"
#undef main
[numthreads(8,8,1)]
void main(uint2 p : SV_DispatchThreadID)
{
    codeInputs(p);
    if (any(p >= P[2].xy)) return;
    uint n = (p.y * P[2].x + p.x + P[2].z * 977u) * 1664525u + 1013904223u;
    n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    RWTexture2D<float4> history = ResourceDescriptorHeap[P[3].x];
    history[p] = float4(n & 255u, (n >> 8) & 255u, (n >> 16) & 255u, n >> 24) / 255.0;
    RWTexture2D<float4> info = ResourceDescriptorHeap[P[3].y];
    info[p] = float4(0, 0, 0, ((p.x + p.y) % 8u) / 8.0);
}
