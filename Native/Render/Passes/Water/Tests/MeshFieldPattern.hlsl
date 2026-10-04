// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= 512 || p.y >= 257) return;
    const uint i = p.y * 512 + p.x, seed = P[0].w;
    const float x = float(p.x) * 0.071 + seed, y = float(p.y) * 0.093 - seed;
    const float amplitude = seed == 0 ? 0 : 0.15;
    RWTexture2D<float4> field = ResourceDescriptorHeap[P[0].x];
    field[p] = float4(amplitude * sin(x) * cos(y), amplitude * cos(x) * cos(y), -amplitude * sin(x) * sin(y), 0);
    RWByteAddressBuffer previous = ResourceDescriptorHeap[P[0].y];
    previous.Store(i * 4, asuint(amplitude * sin(x - 0.02) * cos(y + 0.03)));
    if (i == 0)
    {
        RWByteAddressBuffer centre = ResourceDescriptorHeap[P[0].z];
        centre.Store4(0, asuint(float4(amplitude, 0, amplitude * 0.9, 0)));
    }
}
