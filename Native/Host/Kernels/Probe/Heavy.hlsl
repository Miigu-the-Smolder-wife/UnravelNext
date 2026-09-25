// unx-kernel: cs_6_6 main
// Host boundary probe (ARCHITECTURE 7.1-5): one bandwidth-bound frame pass. Reads the previous ping-pong texture and
// writes the other, so every pass depends on the one before it (a UAV barrier separates them) and costs about one
// full-screen RGBA16F read + write, like the renderer's full-screen passes.
// P[0].x source UAV (RGBA16F), P[0].y destination UAV (RGBA16F), P[0].zw size in pixels, P[1].x pass index. Both
// textures stay in the UNORDERED_ACCESS layout, so passes are separated by global UAV barriers only (like the graph).
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].zw)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> destination = ResourceDescriptorHeap[P[0].y];
    const float k = 1.0 / float(P[1].x + 2);
    destination[p] = source[p] * (1.0 - k) + float4(p.x, p.y, P[1].x, 1) * (k / 4096.0);
}
