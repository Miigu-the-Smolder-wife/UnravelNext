// unx-kernel: cs_6_6 main
#include "Passes/Common/Bindless.hlsli"
[numthreads(16,16,1)]
void main(uint i:SV_GroupIndex)
{
    RWTexture2D<uint4> outMap=ResourceDescriptorHeap[P[0].x];
    uint lane=WaveGetLaneIndex(), count=WaveGetLaneCount();
    outMap[uint2(i%16,i/16)]=uint4(lane,WaveReadLaneAt(i,max(lane,1u)-1),WaveReadLaneAt(i,min(lane+1,count-1)),count);
}
