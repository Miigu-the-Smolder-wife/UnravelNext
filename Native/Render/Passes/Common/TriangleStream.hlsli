// Shared corner ordering for raster, water shading and ray-hit consumers.
#ifndef UNX_TRIANGLE_STREAM_HLSLI
#define UNX_TRIANGLE_STREAM_HLSLI
uint3 triangleStreamVertexIds(uint indexSrv, uint primitive)
{
    const uint first = 3u * primitive;
    if (indexSrv == 0xFFFFFFFFu) return first + uint3(0, 1, 2);
    ByteAddressBuffer indices = ResourceDescriptorHeap[indexSrv];
    return indices.Load3(first * 4u);
}
#endif
