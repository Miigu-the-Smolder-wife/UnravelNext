// unx-kernel: cs_6_6 main
// Deterministic anchors (gi.deterministic): every live entry that has never been updated takes the anchor its candidates'
// minimum encodes (giPackAnchorCandidate), whatever thread created it. Runs after probe placement, before the rays.
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity || (h.flags & 1u) == 0) return;
    const uint2 meta = b.Load2(h.offMeta + entry * 16);
    if (meta.y == 0) return;  // free
    if (b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_UPDATES) != 0) return;
    const uint2 packedWords = b.Load2(h.offAnchorMin + entry * 8);
    if (packedWords.x == 0xFFFFFFFFu && packedWords.y == 0xFFFFFFFFu) return;
    const uint64_t packed = (uint64_t)packedWords.x | ((uint64_t)packedWords.y << 32);
    const uint64_t key = (uint64_t)meta.x | ((uint64_t)meta.y << 32);
    const uint level = (uint)(key & 31u);
    const float s = giCellSize(h, level);
    const int3 cell = (int3(uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu) << 14) >> 14;
    const float3 origin = float3(cell) * s - 0.5 * s;
    const uint3 q = uint3((uint)(packed >> 50) & 16383u, (uint)(packed >> 36) & 16383u, (uint)(packed >> 22) & 16383u);
    const float3 p = origin + float3(q) / 16383.0 * (2 * s);
    const float2 e = float2((uint)(packed >> 11) & 2047u, (uint)packed & 2047u) / 2047.0 * 2 - 1;
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    b.Store4(h.offAnchor + entry * 16, uint4(asuint(p), giPackNormal(normalize(n))));
}
