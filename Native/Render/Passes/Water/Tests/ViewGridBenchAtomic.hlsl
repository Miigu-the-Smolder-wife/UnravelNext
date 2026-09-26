// unx-kernel: cs_6_6 main
// View grid microbench (FEATURES_GAME 1.8 B), 64-bit atomic min throughput on the pixel key buffer:
//   mode 0: fill (every 8 B = P[1].w twice); mode 1: thread i updates pixel i % pixels (screen order, depth complexity =
//   threads / pixels); mode 2: thread i updates a hashed pixel. Key = (hash(i) & 0x7fffffff) << 32 | i.
// P[0] 0, 0, key UAV (raw), 0; P[1] mode, threads, pixels, fill value (mode 0)
#include "Bindless.hlsli"

uint vgHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].y) return;
    RWByteAddressBuffer keys = ResourceDescriptorHeap[P[0].z];
    const uint pixels = P[1].z;
    if (P[1].x == 0) { keys.Store2(i * 8, uint2(P[1].w, P[1].w)); return; }
    const uint pixel = P[1].x == 1 ? i % pixels : vgHash(i ^ 0x9E3779B9u) % pixels;
    keys.InterlockedMin64(pixel * 8, (uint64_t(vgHash(i) & 0x7FFFFFFFu) << 32) | uint64_t(i));
}
