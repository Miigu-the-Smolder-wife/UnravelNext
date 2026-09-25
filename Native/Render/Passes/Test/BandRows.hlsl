// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Render-graph test kernel for banded pass groups: rows [P[1].x, P[1].y) of a P[0].z x P[0].w grid (raw uint buffers).
//   MODE=0 (producer): inter[i] = i * 2654435761
//   MODE=1 (consumer): out[i] = inter[i] ^ 0x5A5A5A5A
//   P[0] inter UAV (MODE 0) / SRV (MODE 1), out UAV (MODE 1), width, height; P[1] y0, y1
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint width = P[0].z, y = P[1].x + id.y;
    if (id.x >= width || y >= P[1].y) return;
    const uint i = y * width + id.x;
#if MODE == 0
    RWByteAddressBuffer inter = ResourceDescriptorHeap[P[0].x];
    inter.Store(4 * i, i * 2654435761u);
#else
    ByteAddressBuffer inter = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer dst = ResourceDescriptorHeap[P[0].y];
    dst.Store(4 * i, inter.Load(4 * i) ^ 0x5A5A5A5Au);
#endif
}
