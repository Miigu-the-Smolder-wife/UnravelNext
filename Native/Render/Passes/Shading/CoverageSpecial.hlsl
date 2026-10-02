// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,4,5,6 AREA=0,1
// Special coverage records (CoverageSpecial.hlsli, INTERFACES 7.1 v1.75), one thread per entry of
// ViewResources::coverageSpecial, dispatched indirectly from its header (64 per group):
//   MODE=0 (before tracks::water): kinds 1 (hair) and 2 (streams) get radiance 0, the defined value their owners
//          overwrite (W: kind 2 in tracks::water; hair: CoverageHair.hlsl before the composite).
//   MODE=1, 2, 4 (before the composite): kind 5 records (COV_PRESHADE_ID) of M's pre-shaded materials, 1 Cut, 2 Terrain,
//          4 A9 layered Standard materials (clearcoat, sheen): the material at the footprint (CoverageShade.hlsli
//          covFragmentMaterial, as the resolve; MODE 4 also the coat's filtered roughness) stored per entry (48 B, M's
//          scratch). A kernel per material kind and the lighting apart: together they exceed the 200 KB DXIL limit.
//   MODE=5, 6: every kind 5 entry lit as the composite lights a cluster record, with the stored material (a layered one
//          with its coat or sheen, COV_COAT): 5 the emission, sun and local lights (unexposed, before the air) into M's second
//          scratch (16 B per entry), 6 the indirect light added, then the air and exposure, into the record radiance
//          (COV_PART 1, 2: one lighting kernel is at the DXIL limit; 2026-09-27 the one-kernel MODE 3 is retired).
// P[0] = { records (StructuredBuffer<uint4>), special list (raw), tile list (raw), MODE 1, 2, 4: material scratch UAV;
// MODE 6: record radiance UAV; MODE 5: direct scratch UAV (raw) }; MODE 1..6: P[1] (visible clusters, M texture
// table); MODE 5, 6: P[3], P[4], P[5].x, P[6].zw, P[7], P[8].x (the shading constants, CoverageShade.hlsli), P[5].y the
// material scratch SRV, MODE 6: P[5].z the direct scratch SRV.
#if MODE == 1 || MODE == 2
#define COV_PRESHADE_CLASSES MODE
#elif MODE == 5 || MODE == 6
#define COV_PRESHADE_LIGHT 1
#define COV_COAT 1
#define MODEL_FILM 1  // A9 thin film (MaterialModel.hlsli modelFresnel)
#define COV_PART (MODE - 4)
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
    const MTriangleIdentity tid = mTriangleIdentity(visId, P[1].x);
    const GpuMaterial m = loadMaterial(tid.material);
    const bool layered = (m.classFlags & MATERIAL_LAYERED) != 0 && materialClass(m) == MATERIAL_STANDARD;
    const uint2 pixel = covRecordPixel(list, entry.x, f);
#if MODE == 5 || MODE == 6
    g_covPreshadeSlot = id.x;
#if MODE == 5
    output.Store3(16 * id.x, asuint(covShadeFragment(visId, entry.x, pixel, P[3].z)));
#else
    output.Store2(entry.x * 8, covPackRadiance(covShadeFragment(visId, entry.x, pixel, P[3].z)));
#endif
#else
#if MODE == 4
    if (!layered) return;
#else
    if (materialClass(m) != (MODE == 1 ? MATERIAL_CUT : MATERIAL_TERRAIN)) return;
#endif
    const MVertex v0 = mTriangleVertex(visId, P[1].x, 0), v1 = mTriangleVertex(visId, P[1].x, 1), v2 = mTriangleVertex(visId, P[1].x, 2);
    const MSurface sf = mSurfaceFromVertices(tid, v0, v1, v2, covFragmentCentre(v0, v1, v2, pixel));
    CovMaterial cm = covFragmentMaterial(visId, sf, m, mLoadTextureSet(P[1].y, tid.material));
#if MODE == 4
    // the coat's (or the sheen's: one layer kind per material) roughness band-limited by the footprint like the base's
    // (MATERIAL_LAYERS 3.4, 1.4; as the resolve)
    const GpuMaterialLayers layers = loadMaterialLayers(m.classFlags >> 16);
    const float ac = modelAlpha((m.classFlags & MATERIAL_SHEEN) != 0 ? layers.sheenRoughness : layers.clearcoatRoughness);
    cm.coatRoughness = min(sqrt(sqrt(ac * ac + cm.variance)), 1.0);
#endif
    covStoreMaterial(output, id.x, cm);
#endif
#endif
}
