// unx-kernel: cs_6_6 main
// r.card.frame (CardLighting.hlsli): one thread. Writes the card frame (CardLayout.hlsli: the indices and sizes every
// reader of the cards needs, behind one SRV) and clears the head and the histograms of an update's select buffer.
// P[0..4] = the frame's words 0..19; P[5] = { card frame UAV (raw), select UAV (raw; 0xFFFFFFFF: none), 0, 0 };
// P[6], P[7] = the frame's words 20..27
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer frame = ResourceDescriptorHeap[P[5].x];
    for (uint i = 0; i < 5; ++i) frame.Store4(i * 16, P[i]);
    frame.Store4(80, P[6]);
    frame.Store4(96, P[7]);
    if (P[5].y == 0xFFFFFFFFu) return;
    RWByteAddressBuffer select = ResourceDescriptorHeap[P[5].y];
    for (uint k = 0; k < CL_SELECT_HEAD + 2u * CL_BUCKETS * 4u; k += 16) select.Store4(k, uint4(0, 0, 0, 0));
}
