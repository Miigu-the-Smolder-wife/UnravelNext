// unx-kernel: cs_6_6 main
// Tile selection already produces compact direct/radiosity lists. Dispatch their
// consumers over those counts, not the atlas update budget. The ray chunks keep
// the same first-thread offsets and worst-case ray bound as the direct path.
// P[0]: select SRV, arguments UAV, direct capacity, radiosity capacity
// P[1]: direct ray description offset, direct chunks, radiosity offset, radiosity chunks
// P[2]: description stride, Width offset, direct threads/chunk, radiosity threads/chunk
// First 64 bytes: four compute descriptions (cull, store, probe, integrate), stride 16.
#include "Bindless.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

void writeRayChunks(RWByteAddressBuffer args, uint count, uint offset, uint chunks, uint perChunk)
{
    for (uint chunk = 0; chunk < chunks; ++chunk)
    {
        const uint first = chunk * perChunk;
        const uint width = count > first ? min(count - first, perChunk) : 0u;
        args.Store3(offset + chunk * P[2].x + P[2].y, width ? uint3(width, 1, 1) : uint3(0, 0, 0));
    }
}

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
    writeRayChunks(args, direct * CL_TRACE_THREADS, P[1].x, P[1].y, P[2].z);
    writeRayChunks(args, radiosity * 64, P[1].z, P[1].w, P[2].w);
}
