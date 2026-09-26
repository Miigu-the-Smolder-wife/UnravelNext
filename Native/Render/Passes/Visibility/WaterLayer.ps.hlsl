// unx-kernel: ps_6_6 main
// Water layer pixels (WaterLayer.ms): the triangle's vis id and the linear view depth at the pixel centre (the depth test
// keeps the nearest water surface in front of band A).
#include "Frame.hlsli"

struct Out
{
    uint visId : SV_Target0;
    float depth : SV_Target1;
};

Out main(float4 position : SV_Position, nointerpolation uint visId : VISID)
{
    Out o;
    o.visId = visId;
    o.depth = linearDepth(position.z);
    return o;
}
