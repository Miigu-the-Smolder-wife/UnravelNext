// unx-kernel: ps_6_6 main
// unx-variants: MODE=0,1
// Classification pages of the local lights (RENDERER_REDESIGN_V2 14.3-1, L3; owner A): the pixel kernel of the
// conservative raster of every active light's six faces at 128 texels (V's depth raster service, UAV-only). Per texel
// the nearest reversed-Z device depth of any caster over the whole texel square: the triangle's depth plane is affine in
// screen space, so its maximum over the square is at a corner, depth(centre) + (|ddx| + |ddy|) / 2 (the derivatives of
// a planar attribute are exact for every pixel of the quad, helper pixels included). Atomic max over the casters
// touching the texel; conservative rasterisation touches every texel a triangle meets, so no caster is missed and
// "no caster nearer than the receiver in its texel" is an exact lit verdict (LocalTileClassify.hlsl). Alpha-tested
// materials cut out as in the main view (depthRasterCovered).
// MODE 1 (14.3-1, stage 2): the exact-raster twin - standard rasterisation, the depth at the texel centre (atomic max:
// the nearest caster at the centre); its blocks' minima settle umbra: every texel of the receiver's blocks holds a
// caster nearer than the receiver (an occluder sampled at the centres; the depth minimum over the block is
// conservative towards lit).
// P[4] = { classification atlas UAV (raw, R32 per texel, 0 = no caster), atlas width in texels, 0, 0 }
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const float z = p.position.z;
#if MODE == 0
    const float zNear = saturate(z + 0.5 * (abs(ddx_fine(z)) + abs(ddy_fine(z))));  // reversed Z: nearer = larger
#else
    const float zNear = saturate(z);
#endif
    RWByteAddressBuffer atlas = ResourceDescriptorHeap[P[4].x];
    const uint2 t = uint2(p.position.xy);
    uint old;
    atlas.InterlockedMax((t.y * P[4].y + t.x) * 4, asuint(zNear), old);  // positive floats order as uints
}
