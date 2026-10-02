// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Diaphragm depth of field (DiaphragmDof.cpp; the reference's DOFSetup.usf, DOFDownsample.usf).
// STEP=0, the setup: per half-resolution pixel its 2 x 2 full-resolution pixels' colour and circle-of-confusion radius
//   (DdofCommon.hlsli ddofCoc of the device depth) into one - the radius of the nearest of the four, the colour by how
//   far behind it each is (ddofDownsample). The gather's input before the prefilter and the reduce.
// STEP=1, the quarter resolution of it (the radius kept: operator 2), which the reduce compares a bright pixel against
//   to decide whether it is scattered (DdofReduce.hlsl).
// P[0] = { colour SRV (STEP=0: full resolution, exposed linear; STEP=1: the half resolution, a = radius),
//          depth SRV (STEP=0), destination UAV (RGBA16F: rgb, a = radius), 0 }
// P[1] = { source width, height, destination width, height }, P[2] = the lens (4 floats, DdofCommon.hlsli)
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#ifndef STEP
#define STEP 0
#endif

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[1].zw)) return;
    Texture2D<float4> source = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> destination = ResourceDescriptorHeap[P[0].z];
    const int2 last = int2(P[1].xy) - 1;
    float3 colour[4];
    float coc[4];
#if STEP == 0
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    const float4 lens = asfloat(P[2]);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const int2 p = min(int2(2 * id) + kDdofSquare[i], last);
        colour[i] = source.Load(int3(p, 0)).rgb;
        coc[i] = ddofCoc(depth.Load(int3(p, 0)), lens);
    }
    float3 c;
    float radius;
    ddofDownsample(colour, coc, true, 1.0, c, radius);
#else
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float4 s = source.Load(int3(min(int2(2 * id) + kDdofSquare[i], last), 0));
        colour[i] = s.rgb;
        coc[i] = s.a;
    }
    float3 c;
    float radius;
    ddofDownsample(colour, coc, false, 1.0, c, radius);
#endif
    destination[id] = float4(c, radius);
}
