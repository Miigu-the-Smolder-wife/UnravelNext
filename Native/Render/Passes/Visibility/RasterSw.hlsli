// V internal: the software rasteriser's triangle (visibility.software_raster; the reference's NaniteRasterizer.ush set-up
// and rectangle loop, here in integers). Owner: V. Used by VisRasterSw.hlsl (the main view's vis buffer) and
// DepthRasterSw.hlsl (the raster service's atlas pages).
// A triangle is given by its vertices in target pixels (x right, y down) with their device depth. The vertices are
// snapped to 1 / 256 px as the hardware rasteriser's 16.8 fixed point, a pixel is covered when its centre lies inside
// under the top-left rule (an edge's bias: 0 for a top or left edge, -1 otherwise), and the depth is the plane's value at
// the pixel centre - so a triangle drawn here covers the pixels the hardware covers for the same snapped vertices, and
// two triangles sharing an edge never both or neither cover a pixel centre on it, whichever path draws each.
// Integers: the clusters this path takes project to at most SW_MAX_PX pixels (the classification's bound), so vertex
// differences stay under 2^14 sub-pixels and every edge product under 2^29.
// Loop bound: the rectangle's rows and columns, each at most SW_MAX_SPAN pixels (a longer one is cut there and the
// kernel reports it: the classification never lets one through; a triangle wider than SW_MAX_PX is not drawn at all).
#ifndef UNX_RASTER_SW_HLSLI
#define UNX_RASTER_SW_HLSLI

#define SW_MAX_PX 64       // the largest cluster rectangle the classification gives this path (pixels)
#define SW_MAX_SPAN 72     // rows / columns one triangle's loop visits at most
#define SW_SUBPIXEL 256

struct SwTriangle
{
    bool valid;      // something to rasterise
    bool cut;        // the rectangle was longer than SW_MAX_SPAN (a defect of the caller's bound)
    int2 lo, hi;     // pixels, inclusive
    int3 edge;       // the three edge functions at pixel lo's centre, the fill rule's bias included: bc, ca, ab
    int3 stepX;      // their change per pixel to the right
    int3 stepY;      // ... and per pixel down
    int3 bias;       // the bias in 'edge' (the depth takes the functions without it)
    float depthA;    // vertex a's depth
    float2 depthBC;  // (depth b - depth a, depth c - depth a) / twice the area
};

// facing: 0 both faces, 1 only triangles whose signed area in y-down pixels is negative (counter-clockwise on screen: the
// front face of an unmirrored view), -1 only those whose area is positive. scissor: pixels [lo, hi] inclusive.
SwTriangle swSetup(float3 v0, float3 v1, float3 v2, int facing, int2 scissorLo, int2 scissorHi)
{
    SwTriangle t = (SwTriangle)0;
    const int2 a = int2(round(v0.xy * SW_SUBPIXEL));
    int2 b = int2(round(v1.xy * SW_SUBPIXEL)), c = int2(round(v2.xy * SW_SUBPIXEL));
    float depthB = v1.z, depthC = v2.z;
    // (wider than the classification's bound - a defect of it, or a vertex that is not a number: the products below
    // would leave 32 bits, so the triangle is not drawn and the kernel reports it)
    if (any(max(a, max(b, c)) - min(a, min(b, c)) > SW_MAX_PX * SW_SUBPIXEL))
    {
        t.cut = true;
        return t;
    }
    int area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0 || (facing > 0 && area > 0) || (facing < 0 && area < 0)) return t;
    if (area < 0)  // the other winding: positive area from here on
    {
        const int2 s = b;
        b = c;
        c = s;
        const float d = depthB;
        depthB = depthC;
        depthC = d;
        area = -area;
    }
    // Pixels whose centre can be inside: centre >= the smallest coordinate, centre < the largest.
    const int2 lowest = min(a, min(b, c)), highest = max(a, max(b, c));
    t.lo = max((lowest + (SW_SUBPIXEL / 2 - 1)) >> 8, scissorLo);
    t.hi = min((highest - (SW_SUBPIXEL / 2 + 1)) >> 8, scissorHi);
    if (any(t.hi - t.lo >= SW_MAX_SPAN))
    {
        t.cut = true;
        t.hi = min(t.hi, t.lo + (SW_MAX_SPAN - 1));
    }
    if (any(t.lo > t.hi)) return t;
    // Edge u -> v at point p: (v.x - u.x)(p.y - u.y) - (v.y - u.y)(p.x - u.x), >= 0 inside for this winding. Top-left: the
    // edge goes up (v.y < u.y: a left edge), or is level and goes right (a top edge).
    const int2 p = t.lo * SW_SUBPIXEL + SW_SUBPIXEL / 2;
    const int2 dBC = c - b, dCA = a - c, dAB = b - a;
    t.bias = int3((dBC.y < 0 || (dBC.y == 0 && dBC.x > 0)) ? 0 : -1, (dCA.y < 0 || (dCA.y == 0 && dCA.x > 0)) ? 0 : -1,
                  (dAB.y < 0 || (dAB.y == 0 && dAB.x > 0)) ? 0 : -1);
    t.edge = int3(dBC.x * (p.y - b.y) - dBC.y * (p.x - b.x), dCA.x * (p.y - c.y) - dCA.y * (p.x - c.x), dAB.x * (p.y - a.y) - dAB.y * (p.x - a.x)) + t.bias;
    t.stepX = -int3(dBC.y, dCA.y, dAB.y) * SW_SUBPIXEL;
    t.stepY = int3(dBC.x, dCA.x, dAB.x) * SW_SUBPIXEL;
    t.depthA = v0.z;
    t.depthBC = float2(depthB - v0.z, depthC - v0.z) / (float)area;
    t.valid = true;
    return t;
}

// The depth at a pixel whose (biased) edge functions are e: vertex b's weight is edge ca, vertex c's edge ab.
float swDepth(SwTriangle t, int3 e)
{
    const int3 u = e - t.bias;
    return t.depthA + (float)u.y * t.depthBC.x + (float)u.z * t.depthBC.y;
}

#endif
