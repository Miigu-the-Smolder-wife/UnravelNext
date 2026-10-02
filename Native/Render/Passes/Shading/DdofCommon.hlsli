// Diaphragm depth of field (shading.dof_diaphragm; DiaphragmDof.cpp has the pass list; the reference's DiaphragmDOF/*):
// shared definitions. A circle-of-confusion radius is signed and in half-resolution pixels (the gather's resolution):
// negative in front of the plane of focus (the foreground), positive behind it. The layers a radius belongs to:
//   foreground    radius under -(DDOF_RECOMBINE_COC - 1): gathered at half resolution, composed over everything else;
//   background    radius above DDOF_RECOMBINE_COC - 1: gathered at half resolution, under the rest;
//   slight        |radius| under DDOF_RECOMBINE_COC: gathered at full resolution by the recombine (DdofRecombine.hlsl),
//                 with a half-resolution gather of the same radii as its stable bound;
//   hole filling  what a blurred foreground uncovers behind itself.
// The layers fade into each other over one pixel of radius around those bounds.
// The radius is the bokeh's vertical one. An anamorphic lens (shading.dof_diaphragm_squeeze, the reference's
// DepthOfFieldSqueezeFactor) has a bokeh narrower by the squeeze: a kernel's sample positions are x / squeeze, a
// picture offset is x * squeeze before it is measured against a radius ("on the lens": the bokeh as a unit shape).
#ifndef UNX_DDOF_COMMON_HLSLI
#define UNX_DDOF_COMMON_HLSLI

#define DDOF_TILE 8                  // half-resolution pixels a radius tile covers (COC_TILE_SIZE)
#define DDOF_LARGE_COC 16384.0       // a radius no pixel has, still a 16-bit float (EXTREMELY_LARGE_COC_RADIUS)
#define DDOF_RECOMBINE_COC 3.0       // the largest |radius| of the full-resolution gather (kMaxSlightOutOfFocusCocRadius)
#define DDOF_FAST_GATHER_ERROR 0.05  // radii within this share of the kernel are one radius (FAST_GATHERING_COC_ERROR)
#define DDOF_LUT 32                  // the bokeh tables' size (BOKEH_LUT_SIZE)
#define DDOF_PI 3.14159265358979
#define DDOF_SCATTER_BYTES 80u       // a scatter list's record: the 2 x 2 pixels' first centre, then 4 x (rgb, |radius|)
#define DDOF_SCATTER_HEADER 16u      // (a scatter list's first word: the records appended)

#define DDOF_FOREGROUND 0
#define DDOF_HOLE_FILLING 1
#define DDOF_BACKGROUND 2
#define DDOF_SLIGHT 3

// The lens (DiaphragmDof.cpp): radius = infinity radius x (1 - focus / z), linear in the reversed device depth
// (1 / z = device / near): lens = { radius at infinity, its slope over the device depth, lowest, highest }.
// The depth blur (shading.dof_diaphragm_depth_blur_radius; the reference's DepthBlurRadius and DepthBlurExponent): a
// blur by distance whatever the focus - radius x (1 - 2^(-z x exponent)), taken where it is wider than the lens's, on
// the lens's side of the focus: blur = { radius (0: none), exponent x near plane }.
float ddofCoc(float deviceDepth, float4 lens, float2 blur)
{
    float coc = lens.x - lens.y * deviceDepth;
    if (blur.x > 0.0)
    {
        const float byDepth = (1.0 - exp2(-blur.y / max(deviceDepth, 1e-9))) * blur.x;
        coc = coc < 0.0 ? -max(-coc, byDepth) : max(coc, byDepth);
    }
    return clamp(coc, lens.z, lens.w);
}

float ddofLuma4(float3 c) { return c.g * 2.0 + (c.r + c.b); }
float ddofRcp(float x) { return x > 0.0 ? rcp(x) : 0.0; }

static const int2 kDdofSquare[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
static const int2 kDdofCross[4] = { int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };

// Four samples into one (the reference's DownsampleSceneColorWithCoc): the radius kept is the nearest surface's
// ('closest': the setup's, operator 4) or the first whose magnitude a later one does not undercut (operator 2), and a
// sample's weight falls with how far behind that radius it is - a foreground edge keeps its colour, the background may
// leak under it (it is hole-filled there anyway).
void ddofDownsample(float3 colour[4], float coc[4], bool closest, float sharpness, out float3 outColour, out float outCoc)
{
    if (closest) outCoc = min(min(coc[0], coc[1]), min(coc[2], coc[3]));
    else
    {
        outCoc = coc[0];
        if (abs(outCoc) > coc[1]) outCoc = coc[1];
        if (abs(outCoc) > coc[2]) outCoc = coc[2];
        if (abs(outCoc) > coc[3]) outCoc = coc[3];
    }
    float3 sum = 0;
    float weights = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float w = saturate(1.0 - (outCoc - coc[i]) * (64.0 * sharpness));
        sum += colour[i] * w;
        weights += w;
    }
    outColour = sum * ddofRcp(weights);
}

// A radius tile (DdofTiles.hlsl): the range of the foreground radii (<= 0) and of the background radii (>= 0) that reach
// it, the smallest background radius a gather from it can meet, and the distance to the nearest closer surface.
struct DdofTile
{
    float fgdMin, fgdMax;
    float bgdMin, bgdMax;
    float bgdMinIntersectable;
    float bgdCloser;
};
DdofTile ddofLoadTile(Texture2D<float4> foreground, Texture2D<float4> background, int2 tile)
{
    const float4 f = foreground.Load(int3(tile, 0)), b = background.Load(int3(tile, 0));
    DdofTile t;
    t.fgdMin = f.x;
    t.fgdMax = f.y;
    t.bgdMax = b.x;
    t.bgdMin = b.y;
    t.bgdMinIntersectable = b.z;
    t.bgdCloser = b.w;
    return t;
}

// What a layer's gather does in a tile (the reference's InferGatherTileSuggestion): nothing ('skip'), the mean of
// its kernel ('plain': every radius in reach is the kernel's within DDOF_FAST_GATHER_ERROR), or the full accumulator.
struct DdofSuggestion
{
    bool skip, plain;
    float minAbs, maxAbs;      // the smallest and the largest |radius|: the largest is the kernel's radius
    float minIntersectable;
    float closest;             // the radius nearest the camera
};
DdofSuggestion ddofSuggest(DdofTile t, uint layer)
{
    DdofSuggestion s;
    s.minIntersectable = DDOF_LARGE_COC;
    if (layer == DDOF_FOREGROUND || layer == DDOF_HOLE_FILLING)
    {
        s.minAbs = -t.fgdMax;
        s.maxAbs = -t.fgdMin;
        s.closest = t.fgdMin;
    }
    else if (layer == DDOF_BACKGROUND)
    {
        s.minAbs = t.bgdMin;
        s.minIntersectable = t.bgdMinIntersectable;
        s.maxAbs = t.bgdMax;
        s.closest = t.bgdMin;
    }
    else
    {
        if (t.fgdMin == 0 && t.bgdMax == 0) s.minAbs = DDOF_LARGE_COC;
        else if (t.fgdMin == 0) s.minAbs = t.bgdMin;
        else if (t.bgdMax == 0) s.minAbs = -t.fgdMax;
        else s.minAbs = min(-t.fgdMax, t.bgdMin);
        s.maxAbs = max(-t.fgdMin, t.bgdMax);
        s.closest = t.fgdMin < 0 ? t.fgdMin : t.bgdMin;
    }
    const float error = s.maxAbs * DDOF_FAST_GATHER_ERROR;
    const float gatherFrom = DDOF_RECOMBINE_COC - 1.0;  // (MinGatherRadius)
    if (layer == DDOF_HOLE_FILLING)  // (only where the foreground's opacity is between 0 and 1)
        s.skip = s.maxAbs - s.minAbs < error || s.minAbs > DDOF_RECOMBINE_COC || s.maxAbs <= gatherFrom;
    else if (layer == DDOF_SLIGHT) s.skip = s.minAbs > DDOF_RECOMBINE_COC;
    else s.skip = s.maxAbs <= gatherFrom;
    s.plain = s.maxAbs - s.minAbs < error;
    return s;
}

// The Petzval stretch (shading.dof_diaphragm_petzval; the reference's DepthOfFieldPetzvalBokeh, its
// CalcPetzvalTransform): away from the picture's centre a bokeh is squashed along the direction to the centre (amount
// > 0: the ovals lie around the centre - the swirl) or across it (amount < 0), to 1 / (1 + |amount| d^falloff) of its
// width; d the distance from a box about the centre in which nothing is squashed (its half extents and its corners'
// radius as a share of the smaller extent), all in the picture's half sizes.
// 'at': the bokeh's centre, -1 .. 1 across the picture (y as the pixels'); petzval = { amount, falloff power, box half
// extents x, y }, more = { corner radius, the picture's width / height }. Returns a symmetric 2 x 2 matrix (rows xy,
// zw): 'toPicture' takes an offset on the lens to the picture (the gathers' sample positions), otherwise a picture
// offset back onto the lens (the sprites). The identity where nothing is squashed.
// (Ours: the axis is the direction in pixels. The reference takes it in the normalised square, which on a wide
// picture leans the ovals away from the circle around the centre.)
float4 ddofPetzval(float2 at, float4 petzval, float2 more, bool toPicture)
{
    const float4 identity = float4(1, 0, 0, 1);
    if (petzval.x == 0.0) return identity;
    const float corner = more.x * min(petzval.z, petzval.w);
    const float2 fromBox = sign(at) * max(abs(at) - petzval.zw + corner, 0.0);
    const float away = length(fromBox);
    if (!(away - corner > 0.0)) return identity;
    const float2 n = normalize(fromBox * float2(more.y, 1.0));
    const float squash = 1.0 + abs(petzval.x) * pow(away - corner, petzval.y);
    const float scale = toPicture ? rcp(squash) : squash;
    const float along = petzval.x > 0.0 ? scale : 1.0, across = petzval.x > 0.0 ? 1.0 : scale;
    const float2 t = float2(n.y, -n.x);
    const float xy = n.x * n.y * along + t.x * t.y * across;
    return float4(n.x * n.x * along + t.x * t.x * across, xy, xy, n.y * n.y * along + t.y * t.y * across);
}
float2 ddofTransform(float4 m, float2 v) { return float2(dot(m.xy, v), dot(m.zw, v)); }

// The bokeh's edge along a direction, as a factor of the radius (1 for a disk; the edge table of DdofBokehLut.hlsl is
// centred on its texel origin and wraps: a direction is looked up 15 texels out).
float ddofEdgeFactor(uint lut, float2 direction)
{
    const float len = length(direction);
    if (lut == 0xFFFFFFFFu || len <= 0.0) return 1.0;
    Texture2D<float4> table = ResourceDescriptorHeap[lut];
    return table.SampleLevel(g_linearWrap, direction * ((0.5 - 1.0 / DDOF_LUT) / len), 0).x;
}

#endif
