// unx-kernel: ps_6_6 main
// Classification pages of the local lights (RENDERER_REDESIGN_V2 14.3-1, L3; owner A): the pixel kernel of the
// conservative raster of every active light's six faces at 128 texels (V's depth raster service, UAV-only). Per texel
// the nearest reversed-Z device depth of any caster over the whole texel square: the triangle's depth plane is affine in
// screen space, so its maximum over the square is at a corner, depth(centre) + (|ddx| + |ddy|) / 2 (the derivatives of
// a planar attribute are exact for every pixel of the quad, helper pixels included). Atomic max over the casters
// touching the texel; conservative rasterisation touches every texel a triangle meets, so no caster is missed and
// "no caster nearer than the receiver in its texel" is an exact lit verdict (LocalTileClassify.hlsl). Alpha-tested
// materials cut out as in the main view (depthRasterCovered).
// P[4] = { classification atlas UAV (raw, R32 per texel, 0 = no caster), atlas width in texels, 0, 0 }
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const float z = p.position.z;
    const float zNear = saturate(z + 0.5 * (abs(ddx_fine(z)) + abs(ddy_fine(z))));  // reversed Z: nearer = larger
    RWByteAddressBuffer atlas = ResourceDescriptorHeap[P[4].x];
    const uint2 t = uint2(p.position.xy);
    uint old;
    atlas.InterlockedMax((t.y * P[4].y + t.x) * 4, asuint(zNear), old);  // positive floats order as uints
}
