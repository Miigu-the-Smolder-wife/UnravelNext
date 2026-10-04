// unx-kernel: cs_6_6 main
// The actual visibility set predicate and shared VSM predicate, not a test copy.
#include "Passes/Visibility/VisibilityCommon.hlsli"
[numthreads(64,1,1)]
void main(uint i:SV_DispatchThreadID)
{
    if(i>=P[0].w)return;
    StructuredBuffer<GpuInstance> instances=ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint2> identities=ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint4> output=ResourceDescriptorHeap[P[0].z];
    const GpuInstance inst=instances[i];const uint2 id=identities[i];
    CullView view=(CullView)0;view.runtimeFirst=id.y;view.instanceSet=1;
    const bool cached=instanceInSet(view,inst,id.x);view.instanceSet=2;
    output[i]=uint4(instanceShadowMovable(inst,id.x,id.y),cached,instanceInSet(view,inst,id.x),0);
}
