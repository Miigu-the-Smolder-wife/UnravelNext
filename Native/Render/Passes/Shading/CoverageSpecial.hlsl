// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3 AREA=0,1
// Special coverage records (CoverageSpecial.hlsli, INTERFACES 7.1 v1.75), one thread per entry of
// ViewResources::coverageSpecial, dispatched indirectly from its header (64 per group):
//   MODE=0 (before tracks::water): kinds 1 (hair) and 2 (streams) get radiance 0, the defined value their owners
//          overwrite (W: kind 2 in tracks::water).
//   MODE=1, 2 (before the composite): kind 5 records (COV_PRESHADE_ID) of M's pre-shaded classes, 1 Cut, 2 Terrain: the
//          material of the record's class at its footprint (CoverageShade.hlsli covFragmentMaterial, as the resolve),
//          stored per entry (48 B, M's scratch). A kernel per class and the lighting apart: together they exceed the
//          200 KB DXIL limit.
//   MODE=3: every kind 5 entry lit as the composite lights a cluster record, with the stored material
//          (COV_PRESHADE_LIGHT), into ViewResources::coverageRecordRadiance.
// P[0] = { records (StructuredBuffer<uint4>), special list (raw), tile list (raw), MODE 1, 2: scratch UAV (raw); MODE 3:
// record radiance UAV (raw) }; MODE 1..3: P[1] (visible clusters, M texture table); MODE 3: P[3], P[4], P[5].x, P[6].zw,
// P[7], P[8].x (the shading constants, CoverageShade.hlsli) and P[5].y the scratch SRV.
#if MODE == 1 || MODE == 2
#define COV_PRESHADE_CLASSES MODE
#elif MODE == 3
#define COV_PRESHADE_LIGHT 1
#endif
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"
#include "Passes/Visibility/CoverageLayer.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer special = ResourceDescriptorHeap[P[0].y];
    if (id.x >= special.Load(0)) return;  // (the count is clamped to the list's capacity by V)
    const uint2 entry = special.Load2(4 * (COV_SPECIAL_HEADER + 2 * id.x));
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].w];
#if MODE == 0
    if (entry.y == COV_SPECIAL_HAIR || entry.y == COV_SPECIAL_STREAM) output.Store2(entry.x * 8, uint2(0, 0));
#else
    if (entry.y != COV_SPECIAL_PRESHADE) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    const CoverageFragment f = coverageUnpackRecord(records[entry.x]);
    const uint visId = coverageClusterVisId(f.visId);
    const uint2 pixel = covRecordPixel(list, entry.x, f);
#if MODE == 3
    g_covPreshadeSlot = id.x;
    output.Store2(entry.x * 8, covPackRadiance(covShadeFragment(visId, entry.x, pixel, P[3].z)));
#else
    const MTriangleIdentity tid = mTriangleIdentity(visId, P[1].x);
    const GpuMaterial m = loadMaterial(tid.material);
    if (materialClass(m) != (MODE == 1 ? MATERIAL_CUT : MATERIAL_TERRAIN)) return;
    const MVertex v0 = mTriangleVertex(visId, P[1].x, 0), v1 = mTriangleVertex(visId, P[1].x, 1), v2 = mTriangleVertex(visId, P[1].x, 2);
    const MSurface sf = mSurfaceFromVertices(tid, v0, v1, v2, covFragmentCentre(v0, v1, v2, pixel));
    covStoreMaterial(output, id.x, covFragmentMaterial(visId, sf, m, mLoadTextureSet(P[1].y, tid.material)));
#endif
#endif
}
