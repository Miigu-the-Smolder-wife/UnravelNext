// unx-kernel: ps_6_6 main
// shadow.vsm.static_separate: the static content of a page into its sampled page. VsmClearPages.ms draws one quad per page
// of a list (the pages drawn anew this frame, then the kept pages whose movable casters were drawn anew); this kernel
// gives each texel the depth of the page's static copy - the same texel of the static atlas, the two atlases hold a page
// at the same place - and the pipeline's depth test (GREATER, reversed Z: nearer the sun) keeps the nearer of it and of
// what the movable casters' raster left there. A depth buffer holds the nearest of the fragments drawn into it in any
// order, and the static and the movable casters are all of a page's casters, so the page then holds what one raster of
// all of them would.
//   P[0] = { page list SRV (VsmClearPages.ms), atlas width, atlas height, static atlas SRV (Texture2D<float>) }
#include "Bindless.hlsli"

float main(float4 position : SV_Position) : SV_Depth
{
    Texture2D<float> staticAtlas = ResourceDescriptorHeap[P[0].w];
    return staticAtlas.Load(int3(position.xy, 0));
}
