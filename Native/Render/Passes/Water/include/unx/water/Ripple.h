#pragma once
// Local ripples W (track W, B7; FEATURES_GAME 1.3 (f); kernels Ripple.hlsli): the linear water surface in a 512^2
// window of 5 cm texels around the focus, evolved every frame in the spectral domain with the exact gravity-capillary
// dispersion over depth d and viscous decay; impulsive sources (physics contacts, footsteps, wakes); an absorbing
// sponge at the window edge; the window follows the focus in whole texels.
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstdint>
#include <vector>

namespace unx::water
{
struct RippleDesc
{
    float texel = 0.05f;            // h (m)
    float depth = 0.0f;             // water depth d (m); 0 = deep water
    float gravity = 9.81f;
    float tensionOverDensity = 7.28e-5f;  // sigma / rho (m^3/s^2): clean water at 20 C
    float viscosity = 1.0e-6f;      // kinematic nu (m^2/s)
    float spongeRate = 20.0f;       // absorption at the window edge (1/s), ramped over the outer 48 texels
    uint32_t maxSources = 1024;     // per frame
    uint32_t framesInFlight = 2;    // upload slots
};
struct RippleSource
{
    float x = 0, z = 0;       // world position (m)
    float radius = 0.05f;     // Gaussian footprint sigma (m); clamped to >= one texel
    float impulse = 0;        // vertical impulse on the water surface (N s, positive = pushed down)
    float volume = 0;         // displaced volume (m^3, positive = water pushed out of the footprint). A body that
                              // floats in water adds, per frame, +V_prev at its previous waterline centre and -V_now at
                              // its current one (NativePhysicsEnvironmentOutput: SubmergedVolume, Point; footprint
                              // sigma about half its waterline radius): splashes and wakes with the volume conserved.
};
struct RippleOutput
{
    // RGBA32F 512^2: (eta, d eta / dx, d eta / dz, phi) at texel (x, z) = world (origin + (x, z) h); metres.
    render::TextureRef field;
    int32_t originTexel[2] = {};  // world texel index of texel (0, 0)
    float texel = 0;
};

class Ripples
{
public:
    static constexpr uint32_t kN = 512, kSponge = 48;
    Ripples(render::Device& device, render::ShaderLibrary& shaders, const RippleDesc& desc);
    ~Ripples();
    Ripples(const Ripples&) = delete;
    Ripples& operator=(const Ripples&) = delete;

    // Advances the surface by dt (s) with the window centred on (focusX, focusZ) and this frame's sources; `frame`
    // selects the source upload slot (frame % framesInFlight).
    RippleOutput record(render::RenderGraph& graph, uint64_t frame, double focusX, double focusZ, float dt, const std::vector<RippleSource>& sources);
    // Replaces the state (eta, phi per texel, row-major, 2 N^2 floats) at the current window; tests and restores.
    void setState(const std::vector<float>& etaPhi);
    // Origin rebase (FrameContext::originShift, C9): world coordinates became the previous ones minus (shiftX, shiftZ);
    // the window keeps its water (its texel origin moves by the same whole number of texels). Each shift must be a whole
    // number of texels (1024 m multiples with the default 5 cm texel).
    void rebase(double shiftX, double shiftZ);
    const RippleDesc& desc() const { return m_desc; }
    static int32_t windowOrigin(double focus, float texel);  // world texel index of texel 0 for a focus coordinate

private:
    render::Device& m_device;
    render::ShaderLibrary& m_shaders;
    RippleDesc m_desc;
    render::ComPtr<ID3D12Resource> m_state, m_accum, m_twiddles, m_output, m_stateUpload;
    std::vector<render::ComPtr<ID3D12Resource>> m_sourceUpload;
    std::vector<uint8_t*> m_sourceMapped;
    std::vector<uint32_t> m_sourceSrv;
    int32_t m_origin[2] = {};
    bool m_placed = false, m_twiddlesUploaded = false, m_stateDirty = false;
};
} // namespace unx::water
