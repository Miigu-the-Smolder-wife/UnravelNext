// unx-kernel: cs_6_6 main
// Every page requested by a pixel also requests the 3 x 3 pages around its ancestors on the next three levels: the
// blocker search and penumbra taps of the visibility pass reach neighbouring pages and fall back to coarser levels,
// and those must be resident. Reads only VSM_REQ_PIXEL, writes only VSM_REQ_PROPAGATED (no order dependence).
// A coarser level with another sun basis (moving sun) takes the page centre, at the level's middle caster height, into
// its own grid (the offset, <= refresh bound x height range, is far below the 3 x 3 pages requested around it).
// P[0].x requests UAV (raw), P[0].y VSM constants CBV, P[0].z unused
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_SUN_SLOTS) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    if ((requests.Load(slot * 4) & VSM_REQ_PIXEL) == 0) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].y];
    const uint k = slot / VSM_SLOTS_PER_LEVEL;
    const int2 page = vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k);
    [loop] for (uint j = 1; j <= 3 && k + j < VSM_LEVELS; ++j)
    {
        float slack;
        const int2 parent = vsmPageAcross(c, page, k, k + j, slack);
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const int2 q = parent + int2(dx, dy);
                if (vsmInWindow(c, q, k + j)) requests.InterlockedOr(vsmSlot(q, k + j) * 4, VSM_REQ_PROPAGATED);
            }
    }
}
