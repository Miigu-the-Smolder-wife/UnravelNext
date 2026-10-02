// Sprite looks (unx/fx/SpriteLooks.h): the renderer's table and its GPU records.
#include "unx/fx/SpriteLooks.h"

#include "unx/core/Log.h"

#include <cmath>
#include <cstring>

namespace unx::fx
{
namespace
{
// ParticleLayerPass.hlsli FxSpriteLook
struct LookGpu
{
    uint32_t texture, normalTexture, motionTexture, flags;
    float motionScale, aspect, stretch, stretchMax;
    float pivot[2], rotationRate, shadowDensity;
    float axis[3], pad0;
    float textureSize[2], pad1, pad2;
};
static_assert(sizeof(LookGpu) == kSpriteLookBytes);

// flags (ParticleLayerPass.hlsli FX_LOOK_*)
constexpr uint32_t kValid = 1u << 0, kLit = 1u << 6, kFrameBlend = 1u << 9, kFramesOverLife = 1u << 10, kSmooth = 1u << 11, kShadow = 1u << 12;
constexpr uint32_t kBlendShift = 1, kFacingShift = 3, kNormalShift = 7, kRibbonUvShift = 13;

bool isColor(scene::TextureFormat f)
{
    return f == scene::TextureFormat::Rgba8Srgb || f == scene::TextureFormat::Rgba8Linear || f == scene::TextureFormat::Rgba16Float;
}
} // namespace

void SpriteLooks::set(uint32_t index, const SpriteLook& l)
{
    if (index >= kMaxSpriteLooks) fail("sprite look %u: the table holds %u looks", index, kMaxSpriteLooks);
    for (float v : { l.motionScale, l.aspect, l.rotationRate, l.stretch, l.stretchMax, l.pivot.x, l.pivot.y, l.shadowDensity, l.axis.x, l.axis.y, l.axis.z })
        if (!std::isfinite(v)) fail("sprite look %u: a value is not finite", index);
    if (!(l.aspect > 0) || l.stretch < 0 || l.stretchMax < 0 || l.shadowDensity < 0) fail("sprite look %u: aspect > 0; stretch, its limit and the shadow density >= 0", index);
    if (l.facing == SpriteFacing::Axis && !(dot(l.axis, l.axis) > 1e-12f)) fail("sprite look %u: the facing axis is zero", index);
    if (l.normal == SpriteNormal::Map && l.normalTexture == kNoTexture) fail("sprite look %u: a normal map look without a normal texture", index);
    if (index >= m_looks.size()) m_looks.resize(index + 1);
    if (!m_looks[index].set) ++m_set;
    m_looks[index] = { l, true };
    ++m_revision;
}

void SpriteLooks::remove(uint32_t index)
{
    if (index >= m_looks.size() || !m_looks[index].set) fail("sprite look %u is not set", index);
    m_looks[index] = {};
    --m_set;
    ++m_revision;
}

void SpriteLooks::clear()
{
    m_looks.clear();
    m_set = 0;
    ++m_revision;
}

bool SpriteLooks::anyShadow() const
{
    for (const Slot& s : m_looks)
        if (s.set && s.look.castShadow) return true;
    return false;
}

std::vector<uint8_t> SpriteLooks::records(const scene::Scene* source, std::span<const uint32_t> textureSrvs) const
{
    std::vector<uint8_t> out(m_looks.size() * sizeof(LookGpu), 0);
    for (size_t i = 0; i < m_looks.size(); ++i)
    {
        const Slot& s = m_looks[i];
        if (!s.set) continue;
        const SpriteLook& l = s.look;
        LookGpu g{};
        g.texture = g.normalTexture = g.motionTexture = kNoTexture;
        bool ready = true;
        // (a texture the look names must be uploaded and of a format its use takes; else the look is unset this frame)
        auto srvOf = [&](uint32_t texture, bool color) -> uint32_t {
            if (texture == kNoTexture) return kNoTexture;
            if (!source || texture >= source->textures.size() || texture >= textureSrvs.size() || textureSrvs[texture] == kNoTexture)
            {
                ready = false;
                return kNoTexture;
            }
            const scene::TextureFormat f = source->textures[texture].format;
            if (color ? !isColor(f) : f != scene::TextureFormat::Rgba8Linear) ready = false;
            return textureSrvs[texture];
        };
        g.texture = srvOf(l.texture, true);
        g.normalTexture = l.normal == SpriteNormal::Map ? srvOf(l.normalTexture, false) : kNoTexture;
        g.motionTexture = l.motionScale != 0 ? srvOf(l.motionTexture, false) : kNoTexture;
        if (!ready) continue;
        if (l.texture != kNoTexture)
        {
            g.textureSize[0] = (float)source->textures[l.texture].width;
            g.textureSize[1] = (float)source->textures[l.texture].height;
        }
        g.flags = kValid | ((uint32_t)l.blend << kBlendShift) | ((uint32_t)l.facing << kFacingShift) | (l.lit ? kLit : 0u) | ((uint32_t)l.normal << kNormalShift) |
                  (l.frameBlend ? kFrameBlend : 0u) | (l.framesOverLife ? kFramesOverLife : 0u) | (l.smooth ? kSmooth : 0u) | (l.castShadow ? kShadow : 0u) |
                  ((uint32_t)l.ribbonUv << kRibbonUvShift);
        g.motionScale = g.motionTexture != kNoTexture ? l.motionScale : 0.0f;
        g.aspect = l.aspect;
        g.stretch = l.stretch;
        g.stretchMax = l.stretchMax;
        g.pivot[0] = l.pivot.x, g.pivot[1] = l.pivot.y;
        g.rotationRate = l.rotationRate;
        g.shadowDensity = l.shadowDensity;
        const float3 axis = l.facing == SpriteFacing::Axis ? normalize(l.axis) : float3{ 0, 1, 0 };
        g.axis[0] = axis.x, g.axis[1] = axis.y, g.axis[2] = axis.z;
        std::memcpy(out.data() + i * sizeof(LookGpu), &g, sizeof g);
    }
    return out;
}

SpriteLooks& spriteLooks(render::TrackState& state) { return state.get<SpriteLooks>("fx.looks"); }
} // namespace unx::fx
