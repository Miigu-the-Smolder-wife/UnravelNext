// The translucency volume's own kernels' view of the grid (LumenTranslucencyVolume.hlsli): cells from the frame constants
// and the root constants every one of them carries:
//   P[4] = { grid x, grid y, grid z, the kernel's own word }
//   P[8] = { asuint(this frame's cell jitter xyz, in cells, [0, 1)), frame }
#ifndef UNX_LUMEN_TRANSLUCENCY_VOLUME_GRID_HLSLI
#define UNX_LUMEN_TRANSLUCENCY_VOLUME_GRID_HLSLI
#include "Frame.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

// P[4] = { grid x, grid y, grid z | log2(the cells' pixel size) << 16, - }
uint3 ltvGridSize() { return uint3(P[4].xy, P[4].z & 0xFFFFu); }
uint ltvPixelShift() { return P[4].z >> 16; }
float ltvPixelSize() { return float(1u << ltvPixelShift()); }
float3 ltvFrameJitter() { return asfloat(P[8].xyz); }
uint ltvFrame() { return P[8].w; }

uint ltvHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float ltvUnit(uint x) { return (ltvHash(x) >> 8) * (1.0 / 16777216.0); }

// World position of a point of the grid given in cells (cell + offset inside it); its view depth.
float3 ltvCellPosition(float3 cell, out float depth)
{
    const float2 pixel = cell.xy * ltvPixelSize();
    depth = ltvDepthOfSlice(cell.z);
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, 1, 1));  // device depth 1 = view depth g_nearPlane
    return g_cameraPosition + (p.xyz / p.w - g_cameraPosition) * (depth / g_nearPlane);
}
float3 ltvCellPosition(float3 cell)
{
    float depth;
    return ltvCellPosition(cell, depth);
}

// Whether the view sees into the cell: its near side (one slice of slack for the readers' trilinear footprint) is in
// front of the farthest depth of its pixels (V's depth pyramid: level 0 is half resolution, so a cell of 2^n pixels is a
// texel of level n - 1).
bool ltvCellVisible(uint3 cell, uint hizSrv)
{
    Texture2D<float> hiz = ResourceDescriptorHeap[hizSrv];
    const float farthest = hiz.Load(int3(cell.xy, ltvPixelShift() - 1u));  // the minimum reversed-Z value
    const float maxDepth = g_nearPlane / max(farthest, 1e-30);
    return ltvDepthOfSlice(max(float(cell.z) - 1.0, 0.0)) < maxDepth;
}

// The cell's sample point kept in front of the depth buffer (GridCenterOffsetFromDepthBuffer 0.5): a sample behind a
// wall would light the air in front of it with what is behind. threshold (slices): the move is made while it is under
// this. The reference's is 1 (OffsetThresholdToAcceptDepthBufferOffset) - a visible cell's sample is up to 2.5 slices
// behind its pixel's surface, so the cell past a wall keeps its sample outside in 3 frames of 4 and the wall's own cell
// in 1 of 8, and the room's volume holds the sky (furnace_room_day: the fog's indirect light). Ours moves every such
// sample (lumen.translucency_volume_depth_offset_threshold = 64): behind a thin object the cell then takes the light in
// front of it on the frames its jittered sample falls on the object.
void ltvDepthConstraint(uint3 cell, inout float3 offset, uint depthSrv, float threshold)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthSrv];
    const uint2 pixel = min(uint2((float2(cell.xy) + offset.xy) * ltvPixelSize()), uint2(g_viewWidth, g_viewHeight) - 1);
    const float sceneDepth = linearDepth(depth.Load(int3(pixel, 0)));
    const float limit = ltvSliceOfDepth(sceneDepth) - 0.5;
    const float delta = limit - (float(cell.z) + offset.z);
    if (delta < 0 && -delta < threshold) offset.z += delta;
}

#endif
