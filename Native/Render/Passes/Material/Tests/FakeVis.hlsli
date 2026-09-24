// M tests only: stand-in for V's band-A visibility until V's vis buffer lands (INTERFACES 5.5.1). Writes the same
// products in the same formats (7.1, v1.5): vis id = packVisId (0 = VIS_NONE), D32 reversed-Z depth, VisibleCluster list.
// Every cluster of every instance is drawn (no culling, no LOD); one-sided back faces are culled per primitive in
// world space; alpha-tested materials test the base colour's mip-0 bilinear alpha at the pixel centre (the reference's
// rule, INTERFACES 8.1).
#ifndef UNX_M_FAKE_VIS_HLSLI
#define UNX_M_FAKE_VIS_HLSLI
#include "Bindless.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

struct FakeVisVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct FakeVisPrimitive
{
    nointerpolation uint visId : VISID;
    nointerpolation uint material : MATERIAL;
};

struct FakeVisPrimitiveOut
{
    nointerpolation uint visId : VISID;
    nointerpolation uint material : MATERIAL;
    bool cull : SV_CullPrimitive;
};

#endif
