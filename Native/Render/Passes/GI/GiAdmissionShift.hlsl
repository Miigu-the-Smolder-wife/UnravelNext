// unx-kernel: cs_6_6 main
// Shift pending request keys under the same whole-cell rule as GiShift. Packed
// anchors are cell-relative. Unrepresentable coarse cells expire, as live ones do.
// P[0]: cache, delta xyz (float bits).
#include "Passes/GI/GiAdmission.hlsli"
[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint i = giAdmissionIndex(group, lane);
    if (i >= min(b.Load(h.offAdmission), b.Load(h.offAdmission + 4))) return;
    const uint a = h.offAdmission + 256 + (h.capacity + i) * 32;
    const uint2 words = b.Load2(a);
    if (words.y == 0) return;
    const uint64_t key = (uint64_t)words.x | ((uint64_t)words.y << 32);
    const float3 cells = asfloat(P[0].yzw) / giCellSize(h, (uint)(key & 31u));
    if (any(cells != round(cells))) { b.Store2(a, 0u); return; }
    const uint3 cell = uint3((uint)(key >> 8), (uint)(key >> 26), (uint)(key >> 44)) & 0x3FFFFu;
    const uint3 moved = uint3(int3(cell) - int3(cells)) & 0x3FFFFu;
    const uint64_t shifted = (key & 0xFFull) | ((uint64_t)moved.x << 8) | ((uint64_t)moved.y << 26) | ((uint64_t)moved.z << 44) | (1ull << 63);
    b.Store2(a, uint2((uint)shifted, (uint)(shifted >> 32)));
}
