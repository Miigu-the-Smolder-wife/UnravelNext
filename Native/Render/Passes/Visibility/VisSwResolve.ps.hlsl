// unx-kernel: ps_6_6 main
// The software rasteriser's pixels into the view's targets (visibility.software_raster; VisRasterSw.hlsl): one triangle
// over the view (PlanarFill.ms); a pixel whose 64-bit word holds a surface writes its depth (SV_Depth) and its vis id
// under the depth test GREATER (reversed Z) with depth write - where the hardware raster's surface is nearer or equal
// it stays. Pixels without a word are discarded. After it the depth buffer, the vis id target and the HiZ built from
// them hold both rasterisers' result.
//   P[0].x the 64-bit words (StructuredBuffer<uint64_t>, width x height), P[0].y width
#include "Bindless.hlsli"

struct Output
{
    uint visId : SV_Target0;
    float depth : SV_Depth;
};

Output main(float4 position : SV_Position)
{
    StructuredBuffer<uint64_t> words = ResourceDescriptorHeap[P[0].x];
    const uint64_t word = words[(uint)position.y * P[0].y + (uint)position.x];
    if (word == 0) discard;
    Output o;
    o.visId = (uint)(word & 0xFFFFFFFFu);
    o.depth = asfloat((uint)(word >> 32));
    return o;
}
