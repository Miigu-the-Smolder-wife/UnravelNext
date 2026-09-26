#pragma once
// Debug drawing, buffer visualization and the pass-time HUD (track E, A15; FEATURES_GAME 7.1). Owner: E.
//
// CPU primitives: debug::drawList(renderer.trackState()) collects lines, points, triangles, boxes, spheres, arrows and
// text for the next recorded frame (immediate mode: the list is uploaded by that frame's debugBegin and cleared). GPU
// primitives: any kernel includes Passes/Debug/DebugDraw.hlsli and calls debugLine / debugTriangle / debugNumber;
// they append to the same buffer (FrameConstants::debugDraw) while debug drawing is on.
// Debug drawing is on in a frame when the quality key debug.draw is true, the HUD (debug.hud) or a buffer view
// (debug.view) is on, or CPU primitives are queued. Off: no pass, no buffer access, g_debugDraw = 0xFFFFFFFF.
// Cost when on (7.1): the draw is one mesh-shader pass of three indirect dispatches over an RGBA16F overlay plus a
// composite (copy of the colour + one read of the overlay); capacities debug.max_lines / max_triangles / max_glyphs
// bound every dispatch (32 primitives per group), appends past them set a status bit (Stats::status) and are dropped.
#include "unx/render/Frame.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace unx::render
{
struct FrameTiming;
}

namespace unx::debug
{
// Primitive flags (DebugDraw.hlsli).
enum Flags : uint32_t
{
    DepthTest = 1,  // hidden where the scene is nearer
    Screen = 2,     // positions are pixels (x, y) of the view; over everything
    XRay = 4,       // with DepthTest: hidden parts at a quarter of the opacity
    Shadow = 8,     // text: a one-pixel dark shadow
};
enum Status : uint32_t
{
    LinesFull = 1,
    TrianglesFull = 2,
    GlyphsFull = 4,
};
// RGBA8, R in the low byte; rgb display-encoded (sRGB), a = opacity.
constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return r | (g << 8) | (b << 16) | ((uint32_t)a << 24); }
uint32_t rgba(float r, float g, float b, float a = 1.0f);

struct Line
{
    float3 a;
    uint32_t color;
    float3 b;
    uint32_t widthFlags;  // width px in 8.8 fixed point | flags << 16
};
struct Triangle
{
    float3 a;
    uint32_t color;
    float3 b;
    uint32_t flags;
    float3 c;
    uint32_t zero;
};
struct Glyph
{
    float3 anchor;
    uint32_t color;
    float2 offset;  // px from the anchor's pixel to the cell's top-left
    uint32_t codeSizeFlags;
    uint32_t zero;
};
static_assert(sizeof(Line) == 32 && sizeof(Triangle) == 48 && sizeof(Glyph) == 32);

class DrawList
{
public:
    void line(float3 a, float3 b, uint32_t color, float widthPx = 1.5f, uint32_t flags = DepthTest);
    void point(float3 p, uint32_t color, float sizePx = 5.0f, uint32_t flags = DepthTest) { line(p, p, color, sizePx, flags); }
    void triangle(float3 a, float3 b, float3 c, uint32_t color, uint32_t flags = DepthTest);
    void box(float3 lo, float3 hi, uint32_t color, float widthPx = 1.5f, uint32_t flags = DepthTest);
    // Three great circles of 'segments' lines each.
    void sphere(float3 centre, float radius, uint32_t color, float widthPx = 1.5f, uint32_t flags = DepthTest, uint32_t segments = 32);
    // Shaft and a four-line head of 'headFraction' of the length.
    void arrow(float3 from, float3 to, uint32_t color, float widthPx = 1.5f, uint32_t flags = DepthTest, float headFraction = 0.2f);
    // Printable ASCII (other bytes are skipped; '\n' starts a new line). sizePx = cell height; cells are sizePx / 2 wide.
    void text(float3 anchor, std::string_view text, uint32_t color, float sizePx = 16.0f, uint32_t flags = DepthTest | Shadow, float2 offsetPx = {});
    void clear();
    bool empty() const { return lines.empty() && triangles.empty() && glyphs.empty(); }

    std::vector<Line> lines;
    std::vector<Triangle> triangles;
    std::vector<Glyph> glyphs;
};

// The renderer's CPU list (FrameRenderer::trackState()).
DrawList& drawList(render::TrackState& state);

// Counts of the last frame whose debug buffer was read back (framesInFlight frames late): appended primitives
// (including those past a capacity) and the status bits.
struct Stats
{
    uint64_t frame = UINT64_MAX;  // UINT64_MAX: none yet
    uint32_t lines = 0, triangles = 0, glyphs = 0, status = 0;
};
Stats lastStats(render::TrackState& state);

// This frame's debug buffer (invalid when debug drawing is off): a pass on the async-compute queue that appends
// declares it as UavCompute so the graph orders it before the draw.
render::BufferRef buffer(render::FramePassContext& fc);

// Buffer visualizations of other tracks (FEATURES_GAME 7.1: each track maps its own buffers): debug.view = name calls
// fn instead of a built-in view; fn writes every pixel of 'colour' (a UAV-capable texture of the view's size and
// format; linearOutput: display light with paper white 1, else display-encoded sRGB). Registered names shadow
// nothing: a name of a built-in view fails.
using ViewFn = std::function<void(render::FramePassContext& fc, const render::ViewResources& view, render::TextureRef colour, bool linearOutput)>;
void registerView(render::TrackState& state, const std::string& name, ViewFn fn);
// Built-in debug.view names (the order is DebugView.hlsl's mode number).
const std::vector<std::string>& builtInViews();

// HUD text of a frame's GPU timings: the frame time, then every pass in execution order with its ms (passes of one
// "<track>." prefix under 1% of the frame are summed into one "<track>.* (n)" line). Lines, no trailing newline.
std::vector<std::string> hudLines(const render::FrameTiming* timing, const Stats& stats);
} // namespace unx::debug
