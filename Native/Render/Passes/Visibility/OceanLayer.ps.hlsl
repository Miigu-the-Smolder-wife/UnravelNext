// unx-kernel: ps_6_6 main
// Ocean in the water layer (v1.73; W's view grid, FEATURES_GAME 1.8 B): drawn with PlanarFill.ms's full-view triangle in
// the water layer's pass, over the same copy of band A's depth as the layer-1 streams (test GREATER, write). Each pixel
// takes W's FrameResources::oceanDepth (linear view depth, +inf = no sea there) as its depth, so the sea behind band A is
// rejected and the nearer of the sea and a layer-1 stream stays. Outputs as WaterLayer.ps: the vis id (the reserved
// ocean slot, COV_OCEAN_ID) and the linear depth.
//   P[0].x oceanDepth SRV (Texture2D<float>)
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

struct Out
{
    uint visId : SV_Target0;
    float depth : SV_Target1;
    float deviceDepth : SV_Depth;
};

Out main(float4 position : SV_Position)
{
    Texture2D<float> ocean = ResourceDescriptorHeap[P[0].x];
    const float d = ocean[uint2(position.xy)];
    if (!(d < asfloat(0x7F800000u)) || !(d > 0)) discard;
    Out o;
    o.visId = COV_OCEAN_ID;
    o.depth = d;
    o.deviceDepth = saturate(g_nearPlane / d);  // reversed Z, infinite far (Frame.hlsli linearDepth inverted)
    return o;
}
