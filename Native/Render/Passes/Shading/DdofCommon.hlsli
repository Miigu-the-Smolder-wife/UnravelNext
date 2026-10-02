// Diaphragm depth of field (shading.dof_diaphragm; DiaphragmDof.cpp has the pass list; the reference's DiaphragmDOF/*):
// shared definitions. A circle-of-confusion radius is signed and in half-resolution pixels (the gather's resolution):
// negative in front of the plane of focus (the foreground), positive behind it. The layers a radius belongs to:
//   foreground    radius under -(DDOF_RECOMBINE_COC - 1): gathered at half resolution, composed over everything else;
//   background    radius above DDOF_RECOMBINE_COC - 1: gathered at half resolution, under the rest;
//   slight        |radius| under DDOF_RECOMBINE_COC: gathered at full resolution by the recombine (DdofRecombine.hlsl),
//                 with a half-resolution gather of the same radii as its stable bound;
//   hole filling  what a blurred foreground uncovers behind itself.
// The layers fade into each other over one pixel of radius around those bounds.
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
float ddofCoc(float deviceDepth, float4 lens) { return clamp(lens.x - lens.y * deviceDepth, lens.z, lens.w); }

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
