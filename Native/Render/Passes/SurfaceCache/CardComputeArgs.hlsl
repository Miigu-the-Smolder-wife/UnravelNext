// unx-kernel: cs_6_6 main
// Compute consumers of the selected tiles. Ray consumers build GPU-counted
// command streams from their own compact queues instead of capacity templates.
#include "Bindless.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint direct = min(select.Load(clSelectContext(0) + CL_SELECT_TILES), P[0].z);
    const uint radiosity = min(select.Load(clSelectContext(1) + CL_SELECT_TILES), P[0].w);
    args.Store4(0, uint4((direct + 63) / 64, 1, 1, 0));
    args.Store4(16, uint4(direct, 1, 1, 0));
    args.Store4(32, uint4((radiosity * 4 + 63) / 64, 1, 1, 0));
    args.Store4(48, uint4(radiosity, 1, 1, 0));
}
