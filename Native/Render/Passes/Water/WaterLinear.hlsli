// Track W: one-dimensional work over a two-dimensional dispatch. D3D12 allows at most 65535 thread groups per dimension
// (a 4K per-pixel pass at 64 threads per group is 129,600), so G groups are dispatched as (min(G, 1024), ceil(G / 1024))
// (unx/water/LinearDispatch.h) and a kernel's linear thread index is waterLinear; the last row's extra groups fall past
// the count and each kernel's own bound check skips them.
#ifndef UNX_WATER_LINEAR_HLSLI
#define UNX_WATER_LINEAR_HLSLI
#define WATER_LINEAR_ROW 1024
uint waterLinear(uint3 group, uint thread, uint groupSize) { return (group.y * WATER_LINEAR_ROW + group.x) * groupSize + thread; }
#endif
