// unx-kernel: cs_6_6 main
// Core test kernel: cofactorNormal (Deformation.hlsli) of one joint 3x3 and normal from root constants.
//   P[0].xyz, P[1].xyz, P[2].xyz rows of the 3x3 (float bits), P[3].xyz normal, P[3].w output UAV (raw, float3)
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Deformation.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    const float3x3 a = float3x3(asfloat(P[0].xyz), asfloat(P[1].xyz), asfloat(P[2].xyz));
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[3].w];
    o.Store3(0, asuint(normalize(cofactorNormal(a, asfloat(P[3].xyz)))));
}
