// The sun's particle transmittance map (FX; fx.particles.shadows; FxShadow.hlsl): what the sprites of the looks that
// cast a shadow (unx/fx/SpriteLooks.h castShadow) let through of the sun, so smoke darkens the ground under it and
// itself. Unreal has a per-object Fourier opacity map for this (translucent shadow depths: optical depth as a Fourier
// series over the light's depth); its shadow maps, virtual ones included, take translucent casters only as an opacity
// clip. S's transmittance layer (VsmLayer.hlsli) is per cached page and has no writer in this tree, and particles
// change every frame: the map is FX's own, in the sun's space, rebuilt every frame.
//
// A square of 'resolution' texels across the sun's direction, centred on the main camera (snapped to the texel grid),
// a texel 'texel' metres wide. Per texel, over the sprites that cover it: the sum of their optical depths there (a
// sprite as a ball: -ln(1 - opacity) of its profile at the texel) and the depth span along the sun's direction that
// their balls' chords take, top to bottom. A receiver at depth z under a texel is behind the part of that optical
// depth above it, taken as uniform over the span: T = exp(-tau saturate((top - z) / (top - bottom))) - 1 above the
// smoke, exp(-tau) below it, and a puff shadows its own lower half by its upper. Read bilinearly (four texels' T).
// A look with a texture: the texel's opacity is the image's alpha there - the flipbook frame of the particle's age, on
// the ball's disc as the sun sees it (the image's own facing, rotation and aspect are the camera's and are not in it).
// Readers: S's screen visibility (the sun's slot), the lit particles, the coverage layer's fragments (the sun profile
// of HairShadow.hlsl MODE 2), the fog's cells and the air's slices.
// Limits: one span a texel (two layers of smoke with clear air between count as one column filled between them);
// the sun only; ray hits do not read it.
#ifndef FX_PARTICLE_SHADOW_HLSLI
#define FX_PARTICLE_SHADOW_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define FX_SHADOW_TAU_UNIT (1.0f / 4096.0f)        // the optical depth sum's fixed point
#define FX_SHADOW_DEPTH_STEPS 16777215.0f          // a depth: (z / depthRange x 0.5 + 0.5) x this, a uint
#define FX_SHADOW_TEXEL_BYTES 12u                  // { tau sum, top (max), bottom (min) }

struct FxShadowParams  // 64 B (raw)
{
    float3 centre; uint resolution;  // the map's centre (world, the frame's origin); texels a side
    float3 axisU; float texel;       // unit axes across the sun's direction; a texel's size (m)
    float3 axisV; float depthRange;  // depths along toSun the map holds: the centre's +- depthRange (m)
    float3 toSun; uint map;          // unit, to the sun; the texels' SRV (raw)
};
FxShadowParams fxShadowParams(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    return b.Load<FxShadowParams>(0);
}
float fxShadowDepth(FxShadowParams p, uint steps) { return ((float)steps / FX_SHADOW_DEPTH_STEPS * 2.0f - 1.0f) * p.depthRange; }
// One texel's transmittance for a receiver at depth z (along toSun from the centre).
float fxShadowTexel(ByteAddressBuffer map, FxShadowParams p, int2 t, float z)
{
    if (any(t < 0) || any(t >= (int)p.resolution)) return 1;
    const uint3 w = map.Load3(FX_SHADOW_TEXEL_BYTES * ((uint)t.y * p.resolution + (uint)t.x));
    if (w.x == 0) return 1;
    const float top = fxShadowDepth(p, w.y), bottom = fxShadowDepth(p, w.z);
    const float part = top > bottom ? saturate((top - z) / (top - bottom)) : (z < top ? 1.0f : 0.0f);
    return exp(-(float)w.x * FX_SHADOW_TAU_UNIT * part);
}
// What the shadow-casting sprites let through of the sun at a world point (the frame's origin). paramsSrv: the map's
// parameters (FrameResources::particleShadowParams; UNX_NONE: no map - 1).
float fxParticleShadow(uint paramsSrv, float3 world)
{
    if (paramsSrv == UNX_NONE) return 1;
    const FxShadowParams p = fxShadowParams(paramsSrv);
    if (p.map == UNX_NONE) return 1;
    ByteAddressBuffer map = ResourceDescriptorHeap[p.map];
    const float3 d = world - p.centre;
    const float2 s = float2(dot(d, p.axisU), dot(d, p.axisV)) / p.texel + 0.5f * p.resolution - 0.5f;
    const float z = dot(d, p.toSun);
    const float2 base = floor(s), f = s - base;
    const int2 t = (int2)base;
    return lerp(lerp(fxShadowTexel(map, p, t, z), fxShadowTexel(map, p, t + int2(1, 0), z), f.x),
                lerp(fxShadowTexel(map, p, t + int2(0, 1), z), fxShadowTexel(map, p, t + int2(1, 1), z), f.x), f.y);
}
#endif
