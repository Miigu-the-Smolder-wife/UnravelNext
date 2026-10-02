// unx-kernel: cs_6_6 main
// r.card.frame (CardLighting.hlsli): one thread. Writes the card frame (CardLayout.hlsli: the indices and sizes every
// reader of the cards needs, behind one SRV) and clears the head and the histograms of the frame's select buffer.
// P[0..4] = the frame's words 0..19; P[5] = { card frame UAV (raw), select UAV (raw), 0, 0 }
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer frame = ResourceDescriptorHeap[P[5].x];
    for (uint i = 0; i < 5; ++i) frame.Store4(i * 16, P[i]);
    RWByteAddressBuffer select = ResourceDescriptorHeap[P[5].y];
    for (uint k = 0; k < CL_SELECT_HEAD + 2u * CL_BUCKETS * 4u; k += 16) select.Store4(k, uint4(0, 0, 0, 0));
}
