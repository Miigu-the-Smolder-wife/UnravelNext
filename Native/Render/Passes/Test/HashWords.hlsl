// unx-kernel: cs_6_6 main
// Render-graph test kernel: a long reader. Each thread first hashes its index P[0].w times (a dependent chain), then
// loads word i of the input and writes input ^ hash to output word i and the hash to output word words + i, so the
// loads happen at the end of a long kernel: a write racing it into the input's memory shows in output ^ hash.
// P[0].x ByteAddressBuffer SRV, P[0].y RWByteAddressBuffer UAV (2 x words), P[0].z words, P[0].w rounds
#include "Bindless.hlsli"

uint hashWord(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    uint k = i;
    for (uint r = 0; r < P[0].w; ++r) k = hashWord(k);
    ByteAddressBuffer input = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    output.Store(4 * i, input.Load(4 * i) ^ k);
    output.Store(4 * (P[0].z + i), k);
}
