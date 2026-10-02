#pragma once
// Sprite looks (FX particle render pass). The VFX stream names two materials for a sprite or ribbon program - 0 emissive
// (colour = nit), 1 lit as a medium (colour = albedo) - and carries a program's flipbook layout (columns, rows, first
// frame, frames per second, uv scale / offset / scroll, rotation curve). What it does not carry is the image and how
// the sprite is drawn: that is a look, a record of the renderer's table. A program whose material is 2 + i is drawn
// with look i; a material past 1 without a look is refused as before (FX_LAYER_STATUS_MATERIAL).
//
// A look gives a sprite (as Unreal's Niagara sprite renderer and its material give one):
//   - a texture (scene::Scene::textures: Rgba8Srgb, Rgba8Linear or Rgba16Float; rgb x the particle's colour, a x its
//     alpha), as a flipbook of the program's columns x rows frames, with the two neighbouring frames blended and,
//     with a motion-vector flipbook, each frame displaced toward the other before the blend;
//   - a blend: alpha, additive, premultiplied;
//   - a facing: the view plane, the camera's position, along the particle's velocity, along a fixed axis; a rotation
//     (the program's rotation curve + a rate), an aspect, a stretch by speed, a pivot;
//   - lighting: emissive, lit as a medium at the particle's centre (the stream's material 1), or lit per pixel with a
//     normal - the sphere's over the sprite, or the flipbook's normal map;
//   - its shadow: the sprite's opacity enters the sun's particle transmittance map (ParticleShadow.hlsl);
//   - for ribbons: the texture along the strip by distance or by age.
#include "unx/core/Math.h"
#include "unx/render/Frame.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <span>
#include <vector>

namespace unx::fx
{
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
constexpr uint32_t kMaxSpriteLooks = 256;
constexpr uint32_t kSpriteLookBytes = 80;  // ParticleLayerPass.hlsli FxSpriteLook

enum class SpriteBlend : uint32_t
{
    Alpha = 0,          // L += T a c, T *= 1 - a
    Additive = 1,       // L += T a c (the sprite hides nothing)
    Premultiplied = 2,  // L += T c, T *= 1 - a: the texture's rgb is already times its alpha
};
enum class SpriteFacing : uint32_t
{
    CameraPlane = 0,     // parallel to the view plane
    CameraPosition = 1,  // its normal points at the camera's position
    Velocity = 2,        // its up axis along the particle's velocity, turned about it toward the camera
    Axis = 3,            // its up axis along 'axis', turned about it toward the camera
};
enum class SpriteNormal : uint32_t
{
    None = 0,       // lit once at the particle's centre with the program's phase function (a medium)
    Spherical = 1,  // lit per pixel with the normal of a sphere over the sprite
    Map = 2,        // lit per pixel with normalTexture (tangent space: +x along the sprite's right, +y along its up)
};
enum class RibbonUv : uint32_t
{
    Distance = 0,  // u = the distance along the strip / the program's ribbon_uv (the texture repeats)
    Age = 1,       // u = the point's age / lifetime: 0 at the head, 1 at the tail, once over the ribbon
};

struct SpriteLook
{
    uint32_t texture = kNoTexture;        // colour and alpha (kNoTexture: the round profile, as the built-in materials)
    uint32_t normalTexture = kNoTexture;  // SpriteNormal::Map: Rgba8Linear, rg = the normal's xy in [0, 1] (z rebuilt), per frame
    uint32_t motionTexture = kNoTexture;  // flipbook motion vectors: Rgba8Linear, rg = (v + 1) / 2, v the frame's motion to
                                          // the next frame in units of motionScale of the frame's uv
    float motionScale = 0;                // uv of a frame per unit of the motion texture's value (0: no motion vectors)
    bool frameBlend = true;               // blend the two frames around the fractional frame (false: the frame floor)
    bool framesOverLife = false;          // the frames once over the particle's life (false: the program's frames_per_second, looping)
    SpriteBlend blend = SpriteBlend::Alpha;
    SpriteFacing facing = SpriteFacing::CameraPlane;
    float3 axis = { 0, 1, 0 };            // SpriteFacing::Axis: the up axis (world)
    float aspect = 1;                     // width / height
    float rotationRate = 0;               // rad / s about the facing direction, added to the program's rotation curve (the camera facings)
    float stretch = 0;                    // SpriteFacing::Velocity: the length grows by this many seconds of travel, x (1 + stretch x speed / size)
    float stretchMax = 0;                 // largest length factor (0: no limit)
    float2 pivot = { 0, 0 };              // the particle's position in the sprite, in half sizes from its centre (0, 0: the centre)
    bool lit = false;                     // colour = albedo (false: colour = radiance, nit)
    SpriteNormal normal = SpriteNormal::None;
    bool smooth = false;                  // the image has no detail below 8 px on screen where the sprite is 80 px or more in
                                          // radius: such sprites may be drawn in the 1/4-resolution layer (the texture at the
                                          // layer's footprint). false: every textured sprite is drawn at full resolution
    bool castShadow = false;              // its opacity enters the sun's particle transmittance map
    float shadowDensity = 1;              // optical depth per unit of the sprite's own (1: as seen)
    RibbonUv ribbonUv = RibbonUv::Distance;
};

class SpriteLooks
{
public:
    void set(uint32_t index, const SpriteLook& look);  // index < kMaxSpriteLooks
    void remove(uint32_t index);
    void clear();
    uint32_t count() const { return (uint32_t)m_looks.size(); }  // the table's length (looks 0 .. count - 1, some unset)
    bool any() const { return m_set > 0; }
    bool anyShadow() const;
    uint64_t revision() const { return m_revision; }

    // The table as GPU records (kSpriteLookBytes each). textureSrvs: per scene texture its SRV (GpuScene::textureSrvs); a
    // look whose texture has no SRV yet, or is of another format than its use takes, is written unset (its programs are
    // refused that frame, not drawn wrongly).
    std::vector<uint8_t> records(const scene::Scene* source, std::span<const uint32_t> textureSrvs) const;

private:
    struct Slot
    {
        SpriteLook look;
        bool set = false;
    };
    std::vector<Slot> m_looks;
    uint32_t m_set = 0;
    uint64_t m_revision = 1;
};

// The renderer's table (FrameRenderer::trackState(), key "fx.looks").
SpriteLooks& spriteLooks(render::TrackState& state);
} // namespace unx::fx
