// unx-kernel: cs_6_6 main
// r.card.feedback.clear (CardLighting.hlsli clFeedback): the feedback table emptied for the frame's hits, after the
// frame before's was copied for the CPU. One thread per hash slot; thread 0 also clears the header.
// P[0] = { feedback table UAV (raw), 0, 0, 0 }
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    if (id.x == 0) table.Store4(0, uint4(0, 0, 0, 0));
    if (id.x < CL_FEEDBACK_SLOTS) table.Store2(CL_FEEDBACK_HEAD + id.x * 8u, uint2(0, 0));
}
