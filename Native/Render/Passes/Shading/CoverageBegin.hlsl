// unx-kernel: cs_6_6 main
// Coverage composite scratch reset (CoverageComposite.hlsl): the allocation counter (word 0) before the tiles take
// their bases. P[0].x = M scratch UAV (raw).
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[P[0].x];
    scratch.Store(0, 0);
}
