#pragma once
// Light functions: cookies, IES profiles, gobos, flicker and light animation (track E, A8; FEATURES_GAME 12). Owner: E.
//
// A light function multiplies a light's emission by f(w, t) (rgb), w the unit direction from the light, t the frame
// time. For delta lights (point, spot) one evaluation is exact (FEATURES_GAME 12): every consumer multiplies the
// light's contribution by LightFunction.hlsli's lightFunction - M's shading, S's froxel in-scattering, R's hit
// shading and GI - so screen, fog, reflections and GI agree. Shadows (VSM pages) do not depend on f.
//   profile (at most one):
//     IES (LM-63, TILT=NONE, type C): candela over the vertical angle theta from the light's forward axis and the
//       horizontal angle phi about it from the light's right axis towards cross(forward, right); stored divided by its
//       peak (the light's intensity is then the peak candela: parseIes reports it); symmetric files (last phi 0, 90,
//       180; 90..270) folded as the standard defines; outside the vertical range 0; bilinear in (phi, theta);
//     cookie: an image projected along the forward axis with half-angle tangents (tanX, tanY) - a spot's slide;
//       outside the image 0, behind the light 0;
//     gobo: an equirectangular image around the light (u = phi / 2 pi, v = theta / pi), for point lights;
//     cookie and gobo images have box-filtered mips; the level is chosen from the receiver's footprint as seen from the
//     light (footprint angle / texel angle), so far receivers are not aliased (condition: the caller's footprint);
//     rotation: the profile turns about the forward axis at rotationSpeed rad/s from rotationPhase;
//   time functions: intensity keys (t, value) and colour keys (t, rgb), piecewise linear, looping with their period
//     (0: held after the last key); flicker: 1 + depth x fractal value noise in time (octaves, frequency Hz, seed),
//     clamped at 0 - deterministic (a hash of the seed and the integer time steps), the same on CPU and GPU.
//   Condition: t is the frame time as float seconds (FrameConstants::time): after ~1 h the flicker's highest octave
//   loses sub-step resolution (ulp 2^-12 s x frequency x 2^(octaves - 1)).
// Position animation moves the light itself (the host's light records): its shadow pages follow the per-frame redraw
// rule (FEATURES_GAME 11), not this function.
#include "unx/render/Frame.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace unx::lights
{
struct IesProfile
{
    std::vector<float> vertical;    // degrees, ascending
    std::vector<float> horizontal;  // degrees, ascending (last 0, 90, 180 or 360)
    std::vector<float> values;      // [v * horizontal.size() + h], divided by peak
    float peak = 0;                 // candela (multiplier applied)
};
// LM-63 text (TILT=NONE, type C photometry); throws on anything else.
IesProfile parseIes(std::string_view text);

struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<float> rgb;  // width x height x 3, linear, row-major from the top
};

enum class Profile : uint32_t
{
    None = 0,
    Ies = 1,
    Cookie = 2,
    Gobo = 3,
};

struct LightFunction
{
    Profile profile = Profile::None;
    IesProfile ies;
    Image image;                        // cookie / gobo
    float tanX = 0.5f, tanY = 0.5f;     // cookie half-angle tangents
    float rotationSpeed = 0, rotationPhase = 0;  // rad/s, rad (about the forward axis)
    std::vector<float2> intensityKeys;  // (t, value); empty = 1
    float intensityPeriod = 0;
    std::vector<float4> colorKeys;      // (t, r, g, b); empty = 1
    float colorPeriod = 0;
    float flickerDepth = 0, flickerFrequency = 8;
    uint32_t flickerOctaves = 3, flickerSeed = 1;
};

// Keyed by the light's index in GpuScene::setLights (the index M, S and R pass to lightFunction).
class LightFunctions
{
public:
    void set(uint32_t light, const LightFunction& f);
    void clear(uint32_t light);
    const LightFunction* get(uint32_t light) const;
    uint64_t revision() const { return m_revision; }                 // any change
    uint64_t imageRevision(uint32_t light) const;                    // a change of that light's image (re-upload)
    uint32_t lights() const { return (uint32_t)m_functions.size(); }

private:
    std::vector<std::unique_ptr<LightFunction>> m_functions;
    std::vector<uint64_t> m_imageRevision;
    uint64_t m_revision = 1;
};

LightFunctions& lightFunctions(render::TrackState& state);

// The image's box-filtered mip chain (level 0 = the image; each level halves, rounding down, to 1 x 1; a coarser texel
// is the exact area mean of the finer texels under its footprint).
std::vector<Image> mipChain(const Image& image);

// The CPU reference of LightFunction.hlsli (same formulas in float; mip level 0 for images): tests and CPU consumers.
float3 evaluate(const LightFunction& f, float3 forward, float3 right, float3 direction, float time);
} // namespace unx::lights
