// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// m.ml.cov.* (shading.mega_lights; Passes/Shading/MegaLights.hlsli; owner A): the surface the coverage layer's MegaLights
// instance works on. V's coverage fragments (CoverageTiles.hlsli: sub-pixel geometry composited over the band A surface)
// took their local lights from a loop over the froxel list with S's shadow maps at the pixel's two depth ends; under
// mega_lights S assigns no local shadow maps. As Unreal's MegaLights runs a second instance on the hair visibility sample
// of each pixel, one more instance (samples, shadow rays, shading, temporal and spatial filter) runs on the pixel's
// nearest opaque cluster fragment, and every fragment of the pixel takes that instance's demodulated diffuse and specular
// light times its own modulation factors (CoverageShade.hlsli).
//   MODE 0  per pixel: nearest depth = 0, element = none.
//   MODE 1  per record (one group per block of V's list, 4 records a thread): the pixel's nearest depth over the opaque
//           cluster records in front of the band A surface (atomic max of the depth bits). Hair and stream records are
//           their owners' (E, W) and are not taken.
//   MODE 2  per record: a record at the pixel's nearest depth stores its element (records of equal depth: the hardware's
//           clip duplicates, identical values - any one).
//   MODE 3  per pixel: the instance's inputs in M's formats - G-buffer (the record's interpolated normal, the material's
//           base colour and roughness without its textures: they shape the sampling and the lobes, the fragments' own
//           colours come back with their factors), material word, device depth. No record: sky.
// P[0] = { records (StructuredBuffer<uint4>), tile list (raw), nearest depth (R32_UINT: MODE 0..2 UAV, MODE 3 SRV),
//          element (R32_UINT: MODE 0, 2 UAV, MODE 3 SRV) }
// P[1] = { band A depth SRV (MODE 1), visible clusters SRV (MODE 3), 0, 0 }
// P[2] = { G-buffer UAV (R32G32_UINT), material word UAV (R32_UINT), depth UAV (R32_FLOAT), 0 } (MODE 3)
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

#if MODE == 0
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(g_viewWidth, g_viewHeight))) return;
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint> element = ResourceDescriptorHeap[P[0].w];
    nearest[id.xy] = 0;
    element[id.xy] = 0xFFFFFFFFu;
}
#elif MODE == 1 || MODE == 2
[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint block = gid.x + gid.y * 65535u;
    if (block >= list.Load(4 * COV_LIST_BLOCKS)) return;  // uniform over the group
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[P[0].z];
    const uint4 info = coverageTileInfo(list, coverageBlockTile(list, block));  // tile, records, record base, block base
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X), first = (block - info.w) * COV_BLOCK;
    const uint2 tileCoord = uint2(info.x % tilesX, info.x / tilesX);
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint i = first + q * 256 + gi;
        if (i >= info.y) continue;
        const CoverageFragment f = coverageUnpackRecord(records[info.z + i]);
        if ((f.visId >> 31) != 0 || !coverageFragmentOpaque(f)) continue;  // hair, streams; see-through records
        const uint p = coverageFragmentPixel(f);
        const uint2 pixel = tileCoord * COV_TILE_PX + uint2(p % COV_TILE_PX, p / COV_TILE_PX);
        if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) continue;
#if MODE == 1
        Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].x];
        if (f.depthBits < asuint(bandDepth[pixel])) continue;  // behind the band A surface: the composite never reaches it
        InterlockedMax(nearest[pixel], f.depthBits);
#else
        RWTexture2D<uint> element = ResourceDescriptorHeap[P[0].w];
        if (nearest[pixel] == f.depthBits) element[pixel] = info.z + i;
#endif
    }
}
#else
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> element = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<uint2> gbuffer = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<uint> words = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[2].z];
    const uint e = element[pixel];
    if (e == 0xFFFFFFFFu)
    {
        gbuffer[pixel] = uint2(0, 0);
        words[pixel] = M_MATERIAL_SKY;
        depth[pixel] = 0;
        return;
    }
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    const CoverageFragment f = coverageUnpackRecord(records[e]);
    const MTriangleIdentity tid = mTriangleIdentity(coverageClusterVisId(f.visId), P[1].y);
    const GpuMaterial m = loadMaterial(tid.material);
    GBufferSample g;
    g.normal = coverageFragmentNormal(f);
    g.baseColor = m.baseColor;
    g.roughness = m.roughness;
    gbuffer[pixel] = encodeGBuffer(g);
    words[pixel] = (tid.material & 0xFFFFu) | (uint(round(saturate(m.metallic) * 255.0)) << 16);
    depth[pixel] = coverageFragmentDepth(f);
}
#endif
