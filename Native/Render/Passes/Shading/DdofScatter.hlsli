// Diaphragm depth of field, the scattered highlights (DiaphragmDof.cpp; the reference's DOFHybridScatter*): what the
// mesh and pixel kernels share. A record of a layer's list (DdofReduce.hlsl) is a 2 x 2 block of half-resolution
// pixels; one quad covers the four discs.
// The lens's shape of a disc (the reference's CocSqueeze, CalcPetzvalTransform and DOFVignetteCommon.ush): a pixel's
// offset from a disc's centre is taken onto the lens - the Petzval matrix of the block (DdofCommon.hlsli ddofPetzval),
// then x * squeeze - and measured there; and the lens's barrel and matte box cut the disc (the vignetting, P[2].z):
//   barrel     a tube of radius P[2].y and length P[2].z in front of the aperture. The rays of a point of the scene fill
//              the aperture (radius P[2].w); at the barrel's end that bundle is a disc of the aperture's radius x
//              (1 - length / the point's distance) about the point's direction x length - off the axis for a point
//              off the picture's centre, where the barrel's rim cuts it (the cat's eye). A pixel's offset over its
//              radius is its place in the aperture: on the picture's side behind the focus, mirrored in front of it;
//   matte box  up to three flags on the barrel's rim (P[6..8]): a flag's far edge, carried back along the point's
//              direction onto the barrel's end, is a line the bundle is cut along.
// Root constants (both stages):
// P[0] = { list SRV (raw: DdofCommon.hlsli DDOF_SCATTER_*), list capacity (records), edge table SRV | none (discs),
//          radius statistics SRV | none (DdofGather.hlsl LAYER=2: the background's occlusion) }
// P[1] = { half width, half height, asuint(radius to circumscribed radius), the bokeh's turn (1, or -1: half a turn -
//          the foreground) }
// P[2] = { asuint(the lens's squeeze), asuint(barrel radius, m), asuint(barrel length, m; < 0: no vignetting),
//          asuint(aperture radius, m) }
// P[3] = { asuint(tan of the half field of view x / squeeze), asuint(y), asuint(focus distance, m), asuint(radius at
//          infinity, half-resolution pixels) }
// P[4] = { asuint(Petzval amount) (0: none), asuint(falloff power), asuint(box half extents x), asuint(y) }
// P[5] = { asuint(the Petzval box's corner radius), asuint(the picture's width / height), 0, 0 }
// P[6..8] = a matte box flag: { asuint(cos), asuint(sin) of its place on the rim (from the picture's right towards its
//          top), asuint(its far edge's distance from the axis, m), asuint(its far edge's distance beyond the barrel's
//          end, m; 0: no flag) }
#ifndef UNX_DDOF_SCATTER_HLSLI
#define UNX_DDOF_SCATTER_HLSLI
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#define DDOF_SPRITES_PER_GROUP 32u
#define DDOF_MATTE_BOX_FLAGS 3u

struct DdofSprite
{
    float4 position : SV_Position;
    nointerpolation float2 centre : CENTRE;  // of the block's first pixel
    nointerpolation float4 s0 : SPRITE0;     // per pixel of the block: rgb (its scattered energy per pixel of its disc), radius
    nointerpolation float4 s1 : SPRITE1;
    nointerpolation float4 s2 : SPRITE2;
    nointerpolation float4 s3 : SPRITE3;
    nointerpolation float4 petzval : PETZVAL;  // picture offsets onto the lens (rows xy, zw)
    // the vignetting, on the barrel's end plane (m; x the picture's right, y its top):
    nointerpolation float2 bundle : BUNDLE;    // the bundle's centre
    nointerpolation float4 spread : SPREAD;    // per pixel of the block: a pixel of offset on the lens, in metres there
    nointerpolation float4 flag01 : FLAG0;     // the flags' far edges carried onto the plane: a point of each (xy, zw)
    nointerpolation float2 flag2 : FLAG1;
};

#endif
