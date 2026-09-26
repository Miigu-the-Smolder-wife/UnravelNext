// unx-kernel: cs_6_6 main
// Test check (VsmTests, LocalShadowTests): the block hierarchy VsmPageMax stored for every resident, valid page (the
// blocks persist with the page), recomputed by brute force from the atlas texels (one lane per block, every texel of the
// block read by that lane):
//   - range: the block's min / max key exactly (min = VSM_EMPTY when a texel is empty), every level;
//   - residual: every non-empty texel's h - ref - plane(x, y) inside the stored [lo, hi], every level (tolerance: float
//     rounding of the terms, 4e-6 relative);
//   - ref: the page's highest caster, the same in every block.
// One group per page table slot; slots that are not resident and valid return.
// P[0].x page table SRV (raw, 8 B per slot), P[0].y atlas SRV (Texture2D<float>), P[0].z blocks SRV (raw), P[0].w VSM
// constants CBV, P[1].x local lights SRV (StructuredBuffer<VsmLocalLight>; 0xFFFFFFFF: none), P[1].z slot count (groups
// (x, y), slot = x + 65535 y), P[1].y output UAV (raw, zeroed):
// [0] pages, [1] range mismatches, [2] residual violations, [3] ref mismatches, [4] largest violation (float bits),
// [5] non-empty texels checked (level 0), [6] sum of level-0 residual widths (units of 1e-5 m), [7] non-empty 8-blocks.
#include "Passes/Shadow/VsmLocal.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> atlas = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer blocks = ResourceDescriptorHeap[P[0].z];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[1].y];
    const uint slot = group.x + group.y * 65535u;
    if (slot >= P[1].z) return;
    const uint e = table.Load(slot * 8);
    if ((e & VSM_FLAG_RESIDENT) == 0 || (e & VSM_FLAG_STALE) != 0) return;
    const uint phys = e & VSM_PHYS_MASK;
    const bool local = slot >= VSM_SUN_SLOTS;
    float a = c.hMin, b = c.hMax;
    if (local)
    {
        StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[1].x];
        const VsmLocalLight l = lights[(slot - VSM_SUN_SLOTS) / VSM_LOCAL_LIGHT_SLOTS];
        a = l.nearM;
        b = l.farM;
    }
    const uint pageBase = phys * VSM_BLOCK_ENTRIES * VSM_BLOCK_BYTES;
    const float ref = blocks.Load<VsmBlock>(pageBase + VSM_BLOCK_OFFSET_128 * VSM_BLOCK_BYTES).ref;
    uint rangeBad = 0, residBad = 0, refBad = 0, texels = 0, width = 0, nonEmpty = 0, pageMax = VSM_EMPTY;
    float worst = 0;
    [loop] for (uint m = 0; m < 5; ++m)
    {
        const uint n = 16u >> m, size = 8u << m;
        if (lane >= n * n) continue;
        const VsmBlock blk = blocks.Load<VsmBlock>(pageBase + (vsmBlockOffset(m) + lane) * VSM_BLOCK_BYTES);
        const uint2 o = uint2(lane % n, lane / n) * size;
        uint lo = 0xFFFFFFFFu, hi = VSM_EMPTY;
        [loop] for (uint t = 0; t < size * size; ++t)
        {
            const uint2 xy = o + uint2(t % size, t / size);
            const float v = atlas.Load(vsmAtlasTexel(phys, xy));
            const uint k = local ? vsmLocalKeyOfDepth(v, a, b) : vsmSunKey(v, a, b);
            lo = min(lo, k);
            hi = max(hi, k);
            if (k == VSM_EMPTY) continue;
            const float2 p = float2(xy) + 0.5;
            const float h = vsmDecode(k);
            const float r = h - ref - (blk.plane.x * p.x + blk.plane.y * p.y + blk.plane.z);
            const float tol = 4e-6 * (abs(h) + abs(ref) + abs(blk.plane.x * p.x) + abs(blk.plane.y * p.y) + abs(blk.plane.z)) + 1e-7;
            const float over = max(blk.residual.x - r, r - blk.residual.y);
            if (over > tol)
            {
                ++residBad;
                worst = max(worst, over);
            }
            if (m == 0) ++texels;
        }
        if (lo != blk.range.x || hi != blk.range.y) ++rangeBad;
        if (blk.ref != ref) ++refBad;
        pageMax = max(pageMax, hi);
        if (m == 0 && hi != VSM_EMPTY)
        {
            ++nonEmpty;
            width += (uint)min((blk.residual.y - blk.residual.x) * 1e5, 1e6);
        }
    }
    if (lane == 0 && ref != (pageMax == VSM_EMPTY ? 0.0 : vsmDecode(pageMax))) ++refBad;
    rangeBad = WaveActiveSum(rangeBad);
    residBad = WaveActiveSum(residBad);
    refBad = WaveActiveSum(refBad);
    texels = WaveActiveSum(texels);
    width = WaveActiveSum(width);
    nonEmpty = WaveActiveSum(nonEmpty);
    worst = WaveActiveMax(worst);
    if (WaveIsFirstLane())
    {
        if (lane == 0) result.InterlockedAdd(0, 1);
        result.InterlockedAdd(4, rangeBad);
        result.InterlockedAdd(8, residBad);
        result.InterlockedAdd(12, refBad);
        result.InterlockedMax(16, asuint(worst));
        result.InterlockedAdd(20, texels);
        result.InterlockedAdd(24, width);
        result.InterlockedAdd(28, nonEmpty);
    }
}
