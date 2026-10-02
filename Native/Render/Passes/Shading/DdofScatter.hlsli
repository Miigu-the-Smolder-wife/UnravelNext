// Diaphragm depth of field, the scattered highlights (DiaphragmDof.cpp; the reference's DOFHybridScatter*): what the
// mesh and pixel kernels share. A record of a layer's list (DdofReduce.hlsl) is a 2 x 2 block of half-resolution
// pixels; one quad covers the four discs.
// Root constants (both stages):
// P[0] = { list SRV (raw: DdofCommon.hlsli DDOF_SCATTER_*), list capacity (records), edge table SRV | none (discs),
//          radius statistics SRV | none (DdofGather.hlsl LAYER=2: the background's occlusion) }
// P[1] = { half width, half height, asuint(radius to circumscribed radius), the bokeh's turn (1, or -1: half a turn) }
#ifndef UNX_DDOF_SCATTER_HLSLI
#define UNX_DDOF_SCATTER_HLSLI
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#define DDOF_SPRITES_PER_GROUP 32u

struct DdofSprite
{
    float4 position : SV_Position;
    nointerpolation float2 centre : CENTRE;  // of the block's first pixel
    nointerpolation float4 s0 : SPRITE0;     // per pixel of the block: rgb (its scattered energy per pixel of its disc), radius
    nointerpolation float4 s1 : SPRITE1;
    nointerpolation float4 s2 : SPRITE2;
    nointerpolation float4 s3 : SPRITE3;
};

#endif
