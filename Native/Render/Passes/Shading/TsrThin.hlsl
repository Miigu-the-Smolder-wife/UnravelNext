// unx-kernel: cs_6_6 main
// m.tsr.thin (Tsr.hlsli; the reference's TSRDetectThinGeometry): geometry thinner than a pixel changes its share of the
// pixel with every jitter offset; the shading rejection would take that for a change of shading and drop the history
// each frame - a wire or a leaf's edge flickers, or is lost. Here such pixels are found and given a relaxation weight:
// m.tsr.reject widens its clamp box by it towards the history's own neighbourhood, so the history stays.
//   coverage   per pixel the share of the pixel its thin geometry covers. The reference counts hits over time (a pixel
//              of a foliage material is 1, else 0) and takes the mean as the coverage; here the coverage layer has the
//              share itself - the opaque fragments' area in front of the opaque surface (UpscaleMotion.hlsl's layers) -
//              and the history is its mean over the jitter: blended 10 % a frame while the 3 x 3 neighbourhood's
//              observation is of the history's distribution (a Student t test of the sums; a drop beyond it, or a
//              neighbourhood with no thin geometry, takes the observation at once).
//   cluster    inside thin geometry (the history's coverage above 0 over 5 x 5; here also a pixel the coverage layer
//              has a thin fragment in now - the reference cannot tell a lone wire from an object's outline, the layer
//              can) the weight peaks where the coverage is near a half - the pixels that are neither the geometry nor
//              its background - and is spread over 3 x 3, scaled by P[2].y (the reference's
//              Coverage.MaxRelaxationWeight);
//   lines      a pixel-wide line of depth: the pixel nearer than both neighbours across it by P[2].x pixel sizes in
//              the world, and so are its neighbours along it (one may miss: diagonals) - a wire, a pole, a branch of the
//              opaque surface itself or of a tracked layer. Its weight is 1, 0.8 on the pixels next to it.
// A pixel under an animated layer (particles, see-through fragments) is not relaxed.
// One group per 16 x 16 internal pixels over a 32 x 32 region in group memory (the chain of 3 x 3 operators reaches 7).
// P[0] = { layers SRV (RGBA8, UpscaleMotion.hlsl; UNX_NONE: no coverage, lines only), depth SRV (the tracked depth),
//          reprojected coverage history SRV (R8), decimate mask SRV (RG8) }
// P[1] = { relaxation UAV (R8), coverage history UAV (R8, next frame's), width, height }
// P[2] = { asuint(depth error multiplier), asuint(max relaxation weight), frame, flags (1: reset) }
// Frame constants b1 = the main view.
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

#define TILE 16
#define BORDER 8
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)
#define NEW_SAMPLE_WEIGHT 0.1

#define CELL_ACROSS_Y 3u      // bits 0..1: nearer than the neighbours above and below (0..2)
#define CELL_ACROSS_X 12u     // bits 2..3: ... left and right
#define CELL_ANIMATED 16u
#define CELL_SAME 32u         // the observation is of the history's distribution
#define CELL_THIN_REGION 64u  // an observation above 0 within 5 x 5

groupshared uint gXH[CELLS];    // the observation and the history (from stage 3: the updated history), 16 bits each
groupshared float gZ[CELLS];    // device depth; from stage 4 the updated history's 3 x 3 minimum
groupshared uint gP[CELLS];     // the CELL_ bits
groupshared uint gAny[CELLS];   // 1: the t value is above 0, 2: the observation is; then their 3 x 3 unions
groupshared uint gSpread[CELLS];
groupshared uint gEW[CELLS];    // the line's edge and the cluster's weight, 16 bits each; stage 6: both spread

uint pack2(float a, float b) { return (uint)(saturate(a) * 65535.0 + 0.5) | ((uint)(saturate(b) * 65535.0 + 0.5) << 16); }
float2 unpack2(uint v) { return float2(v & 0xFFFFu, v >> 16) * (1.0 / 65535.0); }
uint cellIndex(int2 c) { return (uint)(c.y * SIDE + c.x); }
bool inMargin(int2 c, int margin) { return all(c >= margin) && all(c < SIDE - margin); }

float pixelDeviceZError(float deviceZ)
{
    if (!(deviceZ > 0)) return 0;
    const float depth = linearDepth(deviceZ);
    const float radius = depth * g_tanHalfFovY / g_viewHeight;
    return abs(g_nearPlane / (depth + 2.0 * radius) - deviceZ);
}

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 size = int2(P[1].zw);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> history = ResourceDescriptorHeap[P[0].z];
    const bool cut = (P[2].w & 1u) != 0;
    uint i;

    // 0: the region's observation, history, depth
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 p = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float observed = 0, animated = 0;
        if (P[0].x != UNX_NONE)
        {
            Texture2D<float4> layers = ResourceDescriptorHeap[P[0].x];
            const float4 layer = layers.Load(int3(p, 0));
            observed = layer.r;
            animated = layer.g;
        }
        gXH[i] = pack2(observed, cut ? 0.0 : history.Load(int3(p, 0)));
        gZ[i] = depth.Load(int3(p, 0));
        gP[i] = animated > 0.25 ? CELL_ANIMATED : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // 1: the depth steps across the pixel; the t test of the observation against the history over 3 x 3
    const float errorMultiplier = asfloat(P[2].x);
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 1)) continue;
        const float z = gZ[i], threshold = pixelDeviceZError(z) * errorMultiplier;
        const uint above = z - gZ[cellIndex(c + int2(0, -1))] > threshold ? 1u : 0u, below = z - gZ[cellIndex(c + int2(0, 1))] > threshold ? 1u : 0u;
        const uint left = z - gZ[cellIndex(c + int2(-1, 0))] > threshold ? 1u : 0u, right = z - gZ[cellIndex(c + int2(1, 0))] > threshold ? 1u : 0u;
        float sumObserved = 0, sumHistory = 0, sumVariance = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gXH[cellIndex(c + int2(k % 3, k / 3) - 1)]);
            sumObserved += v.x;
            sumHistory += v.y;
            sumVariance += v.y * (1.0 - v.y);
        }
        // (only a drop of the coverage is tested: a rise takes the observation at once, stage 3)
        const float excess = sumHistory - sumObserved;
        const float deviation = sqrt(sumVariance + 1e-6) * sqrt(NEW_SAMPLE_WEIGHT / (2.0 - NEW_SAMPLE_WEIGHT) * 72.0);
        const float tValue = excess > 0 ? excess / deviation : 0.0;
        const bool same = tValue < 20.754;  // 2.306 (p = 0.05, 8 degrees of freedom) x 9
        gP[i] = (gP[i] & CELL_ANIMATED) | (above + below) | ((left + right) << 2) | (same ? CELL_SAME : 0u);
        gAny[i] = (tValue > 0 ? 1u : 0u) | (unpack2(gXH[i]).x > 0 ? 2u : 0u);
    }
    GroupMemoryBarrierWithGroupSync();

    // 2: the lines (the pixel and its neighbours along the line are nearer than both sides); the unions over 3 x 3
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 2)) continue;
        const uint own = gP[i];
        const float alongX = (float)((gP[cellIndex(c + int2(-1, 0))] & CELL_ACROSS_Y) + (gP[cellIndex(c + int2(1, 0))] & CELL_ACROSS_Y));
        const float alongY = (float)(((gP[cellIndex(c + int2(0, -1))] & CELL_ACROSS_X) + (gP[cellIndex(c + int2(0, 1))] & CELL_ACROSS_X)) >> 2);
        // (a horizontal line: the pixel is nearer than above and below, its left and right neighbours by 3 of their 4 steps)
        float lineEdge = 0;
        if ((own & CELL_ACROSS_Y) == 2u) lineEdge += saturate((alongX - 2.0) * 0.5);
        if ((own & CELL_ACROSS_X) == 8u) lineEdge += saturate((alongY - 2.0) * 0.5);
        if ((own & CELL_ANIMATED) != 0) lineEdge = 0;
        uint both = 0;
        [unroll] for (int k = 0; k < 9; ++k) both |= gAny[cellIndex(c + int2(k % 3, k / 3) - 1)];
        gEW[i] = pack2(lineEdge, 0);
        gSpread[i] = both;
    }
    GroupMemoryBarrierWithGroupSync();

    // 3: the unions over 5 x 5; the history's update
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 3)) continue;
        uint both = 0;
        [unroll] for (int k = 0; k < 9; ++k) both |= gSpread[cellIndex(c + int2(k % 3, k / 3) - 1)];
        const bool notAbsoluteMatch = (both & 1u) != 0, thinRegion = (both & 2u) != 0;
        const uint own = gP[i];
        const bool accept = thinRegion && notAbsoluteMatch && (own & CELL_SAME) != 0 && (own & CELL_ANIMATED) == 0;
        const float2 xh = unpack2(gXH[i]);
        gXH[i] = pack2(xh.x, lerp(xh.y, xh.x, accept ? NEW_SAMPLE_WEIGHT : 1.0));
        gP[i] = own | (thinRegion ? CELL_THIN_REGION : 0u);
    }
    GroupMemoryBarrierWithGroupSync();

    // 4, 5: the updated history's minimum over 5 x 5; the cluster's weight
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 4)) continue;
        float lo = 1;
        [unroll] for (int k = 0; k < 9; ++k) lo = min(lo, unpack2(gXH[cellIndex(c + int2(k % 3, k / 3) - 1)]).y);
        gZ[i] = lo;
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5)) continue;
        float lo = 1;
        [unroll] for (int k = 0; k < 9; ++k) lo = min(lo, gZ[cellIndex(c + int2(k % 3, k / 3) - 1)]);
        // x exp(-20 x) (10 / 0.184) with x = the coverage's distance from a half + 0.02: 1 at x = 0.05
        const uint own = gP[i];
        const float2 xh = unpack2(gXH[i]);
        const float x = abs(xh.y - 0.5) + 0.02;
        const bool relax = (lo > 0 || xh.x > 0) && (own & (CELL_THIN_REGION | CELL_SAME | CELL_ANIMATED)) == (CELL_THIN_REGION | CELL_SAME);
        gEW[i] = pack2(unpack2(gEW[i]).x, relax ? x * exp(-20.0 * x) * (10.0 / 0.184) : 0.0);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: both spread over 3 x 3 (0.8, 1, 0.8 each way), the cluster's weight under its scale
    const float maxRelaxation = asfloat(P[2].y);
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6)) continue;
        float2 sum = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 o = int2(k % 3, k / 3) - 1;
            sum += unpack2(gEW[cellIndex(c + o)]) * ((o.x == 0 ? 1.0 : 0.8) * (o.y == 0 ? 1.0 : 0.8));
        }
        gSpread[i] = pack2(sum.x, sum.y * maxRelaxation);
    }
    GroupMemoryBarrierWithGroupSync();

    // 7: this thread's pixel: the cluster's weight spread once more (0.5, 1, 0.5)
    const int2 pixel = int2(group) * TILE + int2(local);
    if (any(pixel >= size)) return;
    const int2 cell = int2(local) + BORDER;
    float cluster = 0;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const int2 o = int2(k % 3, k / 3) - 1;
        cluster += unpack2(gSpread[cellIndex(cell + o)]).y * ((o.x == 0 ? 1.0 : 0.5) * (o.y == 0 ? 1.0 : 0.5));
    }
    const uint ci = cellIndex(cell);
    const float lineEdge = unpack2(gSpread[ci]).x, coverage = unpack2(gXH[ci]).y;
    Texture2D<float2> decimateMask = ResourceDescriptorHeap[P[0].w];
    const bool disoccluded = ((uint)round(decimateMask.Load(int3(pixel, 0)).r * 255.0) & 3u) != 0;
    // (a line's weight first; the cluster's only where the pixel has coverage itself)
    float weight = lineEdge > 0 ? lineEdge : (coverage > 0.99 / 127.0 ? saturate(cluster) : 0.0);
    if ((gP[ci] & CELL_ANIMATED) != 0 || disoccluded) weight = 0;
    RWTexture2D<float> relaxation = ResourceDescriptorHeap[P[1].x];
    relaxation[pixel] = weight;
    // the coverage rounded to its 8 bits at random (no drift of the mean under the blend)
    uint3 h = uint3(pixel, P[2].z) * uint3(1664525u, 22695477u, 2891336453u) + 1013904223u;
    h.x += h.y * h.z;
    h ^= h >> 16;
    RWTexture2D<float> historyOut = ResourceDescriptorHeap[P[1].y];
    historyOut[pixel] = min(floor(coverage * 255.0 + (float)(h.x & 0xFFFFu) / 65536.0), 255.0) / 255.0;
}
