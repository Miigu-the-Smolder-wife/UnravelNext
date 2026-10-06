// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].zw)) return;
    RWTexture2D<float4> a = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> b = ResourceDescriptorHeap[P[0].y];
    uint v = (p.x + 1) * 1664525u ^ (p.y + P[1].x) * 1013904223u;
    float4 value = float4((v & 65535u) / 1023.0, ((v >> 3) & 4095u) / 8191.0,
                         ((v >> 13) & 8191u) / 65537.0, (v & 255u) / 255.0);
    if (((p.x / 31 + p.y + P[1].x) & 7u) == 0) value = 0;
    a[p] = b[p] = value;
}
