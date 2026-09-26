// Debug drawing (FEATURES_GAME 7.1; A15). Owner: E. Readers: any kernel (the append API below), E's draw passes.
//
// One raw buffer per renderer, FrameConstants::debugDraw its UAV (g_debugDraw; 0xFFFFFFFF = debug drawing off, every
// append is a no-op). Cleared at the start of each frame (E's debug.begin pass, which also copies the CPU-queued
// primitives in), drawn at its end (debugOverlay) over the main view's colour. Appends from any pass on the graphics
// queue are drawn: the draw pass starts with a global barrier. A pass on the async-compute queue that appends declares
// debug::buffer(fc) as a UAV so the graph orders it.
//
// Layout (bytes):
//   header, 256 B: [0] lines appended, [4] triangles appended, [8] glyphs appended (appends past a capacity are
//   counted and dropped, and set a status bit: nothing is clamped silently), [12] status bits, [16] line capacity,
//   [20] triangle capacity, [24] glyph capacity, [28] CPU lines, [32..63] draw arguments (DebugArgs.hlsl): mesh groups
//   (x, y, z) of lines at 32, triangles at 44, glyphs at 56 (12 B each; 32 primitives per group)
//   lines     at 256:                              32 B each: float3 a; uint colour; float3 b; uint width | flags
//   triangles at 256 + 32 lineCapacity:            48 B each: float3 a; uint colour; float3 b; uint flags; float3 c; uint 0
//   glyphs    after the triangles:                 32 B each: float3 anchor; uint colour; float2 offset (px); uint code |
//                                                  size | flags; uint 0
// Positions are world space (the frame's viewProj), or pixels (x, y) of the view with DEBUG_SCREEN. Colours are RGBA8,
// R in the low byte: rgb display-encoded (sRGB, as an artist picks it), a = opacity. Line widths are pixels in 8.8
// fixed point (a point is a line of zero length: a round dot of that diameter). Glyph size = cell height in pixels.
#ifndef UNX_DEBUG_DRAW_HLSLI
#define UNX_DEBUG_DRAW_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"

#define DEBUG_DRAW_OFF 0xFFFFFFFFu
#define DEBUG_HEADER_BYTES 256u
#define DEBUG_LINE_BYTES 32u
#define DEBUG_TRIANGLE_BYTES 48u
#define DEBUG_GLYPH_BYTES 32u
#define DEBUG_PER_GROUP 32u
#define DEBUG_GLYPH_ASPECT 0.5f     // glyph cell width / height (monospace; the font atlas's cells are 1:2)
#define DEBUG_H_LINES 0u
#define DEBUG_H_TRIANGLES 4u
#define DEBUG_H_GLYPHS 8u
#define DEBUG_H_STATUS 12u
#define DEBUG_H_LINE_CAPACITY 16u
#define DEBUG_H_TRIANGLE_CAPACITY 20u
#define DEBUG_H_GLYPH_CAPACITY 24u
#define DEBUG_H_CPU_LINES 28u
#define DEBUG_H_ARGS_LINES 32u
#define DEBUG_H_ARGS_TRIANGLES 44u
#define DEBUG_H_ARGS_GLYPHS 56u

// Primitive flags.
#define DEBUG_DEPTH_TEST 1u   // hidden where the scene is nearer (1e-3 relative depth tolerance: lines on a surface show)
#define DEBUG_SCREEN 2u       // positions are pixels (x, y); drawn over everything
#define DEBUG_XRAY 4u         // with DEBUG_DEPTH_TEST: hidden parts drawn at a quarter of the opacity instead
#define DEBUG_SHADOW 8u       // glyphs: a one-pixel dark shadow (readable on any background)

// Status bits.
#define DEBUG_STATUS_LINES_FULL 1u
#define DEBUG_STATUS_TRIANGLES_FULL 2u
#define DEBUG_STATUS_GLYPHS_FULL 4u

uint debugPackColor(float4 c)
{
    const uint4 u = uint4(round(saturate(c) * 255.0f));
    return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24);
}
float4 debugUnpackColor(uint c) { return float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) / 255.0f; }
bool debugOn() { return g_debugDraw != DEBUG_DRAW_OFF; }

uint debugTrianglesOffset(uint lineCapacity) { return DEBUG_HEADER_BYTES + lineCapacity * DEBUG_LINE_BYTES; }
uint debugGlyphsOffset(uint lineCapacity, uint triangleCapacity) { return debugTrianglesOffset(lineCapacity) + triangleCapacity * DEBUG_TRIANGLE_BYTES; }

// Reserves n consecutive records of a kind; returns false (and sets the status bit) when they do not fit.
bool debugReserve(RWByteAddressBuffer b, uint counter, uint capacityWord, uint fullBit, uint n, out uint first)
{
    b.InterlockedAdd(counter, n, first);
    if (first + n <= b.Load(capacityWord)) return true;
    b.InterlockedOr(DEBUG_H_STATUS, fullBit);
    return false;
}

void debugLine(float3 a, float3 b, float4 color, float widthPx = 1.5f, uint flags = DEBUG_DEPTH_TEST)
{
    if (!debugOn()) return;
    RWByteAddressBuffer buf = ResourceDescriptorHeap[g_debugDraw];
    uint i;
    if (!debugReserve(buf, DEBUG_H_LINES, DEBUG_H_LINE_CAPACITY, DEBUG_STATUS_LINES_FULL, 1, i)) return;
    const uint o = DEBUG_HEADER_BYTES + i * DEBUG_LINE_BYTES;
    const uint width = min(uint(round(max(widthPx, 0.0f) * 256.0f)), 0xFFFFu);
    buf.Store4(o, uint4(asuint(a), debugPackColor(color)));
    buf.Store4(o + 16, uint4(asuint(b), width | (flags << 16)));
}
void debugPoint(float3 p, float4 color, float sizePx = 5.0f, uint flags = DEBUG_DEPTH_TEST) { debugLine(p, p, color, sizePx, flags); }
void debugTriangle(float3 a, float3 b, float3 c, float4 color, uint flags = DEBUG_DEPTH_TEST)
{
    if (!debugOn()) return;
    RWByteAddressBuffer buf = ResourceDescriptorHeap[g_debugDraw];
    uint i;
    if (!debugReserve(buf, DEBUG_H_TRIANGLES, DEBUG_H_TRIANGLE_CAPACITY, DEBUG_STATUS_TRIANGLES_FULL, 1, i)) return;
    const uint o = debugTrianglesOffset(buf.Load(DEBUG_H_LINE_CAPACITY)) + i * DEBUG_TRIANGLE_BYTES;
    buf.Store4(o, uint4(asuint(a), debugPackColor(color)));
    buf.Store4(o + 16, uint4(asuint(b), flags));
    buf.Store4(o + 32, uint4(asuint(c), 0));
}
// Axis-aligned box as its 12 edges.
void debugBox(float3 lo, float3 hi, float4 color, float widthPx = 1.5f, uint flags = DEBUG_DEPTH_TEST)
{
    [unroll] for (uint e = 0; e < 12; ++e)
    {
        const uint axis = e / 4, k = e % 4;  // edge along 'axis', the other two coordinates from k's bits
        float3 p = lo, q = lo;
        const uint u = (axis + 1) % 3, v = (axis + 2) % 3;
        if (k & 1) { p[u] = hi[u]; q[u] = hi[u]; }
        if (k & 2) { p[v] = hi[v]; q[v] = hi[v]; }
        q[axis] = hi[axis];
        debugLine(p, q, color, widthPx, flags);
    }
}

// Text: glyphs of printable ASCII (32..126) at anchor + offset, the offset in pixels from the anchor's pixel (x right,
// y down) to the glyph cell's top-left corner.
void debugGlyphs(float3 anchor, float2 offsetPx, uint4 codes, uint count, float sizePx, float4 color, uint flags)
{
    if (!debugOn() || count == 0) return;
    RWByteAddressBuffer buf = ResourceDescriptorHeap[g_debugDraw];
    uint i;
    if (!debugReserve(buf, DEBUG_H_GLYPHS, DEBUG_H_GLYPH_CAPACITY, DEBUG_STATUS_GLYPHS_FULL, count, i)) return;
    const uint base = debugGlyphsOffset(buf.Load(DEBUG_H_LINE_CAPACITY), buf.Load(DEBUG_H_TRIANGLE_CAPACITY));
    const uint size = min(uint(round(max(sizePx, 1.0f))), 255u);
    const uint packed = debugPackColor(color);
    const float advance = sizePx * DEBUG_GLYPH_ASPECT;
    for (uint k = 0; k < count; ++k)
    {
        const uint code = (codes[k / 4] >> (8 * (k % 4))) & 0xFFu;
        const uint o = base + (i + k) * DEBUG_GLYPH_BYTES;
        buf.Store4(o, uint4(asuint(anchor), packed));
        buf.Store4(o + 16, uint4(asuint(offsetPx + float2(advance * k, 0)), code | (size << 8) | (flags << 16), 0));
    }
}
// A decimal number (up to 16 characters): integers below 1e7 exactly; other values with 4 significant digits, fixed
// point in [1e-3, 1e7) and scientific (d.ddde+XX) outside; "nan", "inf".
static const uint kDebugPow10[8] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u };
void debugNumber(float3 anchor, float2 offsetPx, float value, float sizePx = 16.0f, float4 color = float4(1, 1, 1, 1), uint flags = DEBUG_DEPTH_TEST | DEBUG_SHADOW)
{
    // character codes: '0' 48, '-' 45, '+' 43, '.' 46, 'e' 101, 'i' 105, 'n' 110, 'f' 102, 'a' 97
    uint chars[16];
    uint n = 0;
    [unroll] for (uint z = 0; z < 16; ++z) chars[z] = 32u;
    if (isnan(value)) { chars[0] = 110u; chars[1] = 97u; chars[2] = 110u; n = 3; }
    else
    {
        if (value < 0) { chars[n++] = 45u; value = -value; }
        if (isinf(value)) { chars[n++] = 105u; chars[n++] = 110u; chars[n++] = 102u; }
        else
        {
            int exponent = 0;
            const bool scientific = value != 0 && (value >= 1e7f || value < 1e-3f);
            if (scientific)
            {
                exponent = (int)floor(log10(value));
                value = value * exp2(-exponent * 3.3219280948873623f);  // / 10^exponent
                if (value >= 9.9995f) { value *= 0.1f; exponent += 1; }
                if (value < 1.0f) { value *= 10.0f; exponent -= 1; }
            }
            const bool integral = !scientific && value == floor(value);
            // 4 significant digits: decimals = 3 - floor(log10(value)) in fixed point (value >= 1e-3: at most 6)
            const uint decimals = integral ? 0u : (scientific ? 3u : (uint)clamp(3 - (int)floor(log10(value)), 0, 6));
            const uint scale = kDebugPow10[decimals];
            uint whole = (uint)floor(value);
            uint fraction = (uint)round((value - whole) * scale);
            if (fraction >= scale) { fraction -= scale; whole += 1; }
            uint digits[8];
            uint d = 0;
            [loop] do { digits[d++] = whole % 10u; whole /= 10u; } while (whole != 0 && d < 8);
            [loop] for (uint k = d; k > 0; --k) chars[n++] = 48u + digits[k - 1];
            if (decimals > 0)
            {
                chars[n++] = 46u;
                [loop] for (int k = (int)decimals - 1; k >= 0; --k) chars[n++] = 48u + (fraction / kDebugPow10[k]) % 10u;
            }
            if (scientific)
            {
                chars[n++] = 101u;
                chars[n++] = exponent < 0 ? 45u : 43u;
                const uint e = (uint)abs(exponent);
                chars[n++] = 48u + (e / 10u) % 10u;
                chars[n++] = 48u + e % 10u;
            }
        }
    }
    uint4 codes = 0;
    [unroll] for (uint k = 0; k < 16; ++k) codes[k / 4] |= chars[k] << (8 * (k % 4));
    debugGlyphs(anchor, offsetPx, codes, min(n, 16u), sizePx, color, flags);
}
#endif
