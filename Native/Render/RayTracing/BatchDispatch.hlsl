// unx-kernel: cs_6_6 main
// GPU-owned ray command stream. Count at byte 0, records at byte 16:
// { first-thread root constant, tightly packed D3D12_DISPATCH_RAYS_DESC }.
// P0={counter SRV, arguments UAV, template SRV, item capacity}
// P1={threads/item, threads/command, command capacity, counter byte offset}
// P2={template byte offset, record stride, description bytes, Width byte offset}
// P3.x=optional row width. Then the root constant is the first row and the
// dispatch is (row width, live rows, 1); command boundaries must align to rows.
// P3.yz=constant flag bits and first-thread divisor (e.g. a packed band index).
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint command : SV_DispatchThreadID)
{
    ByteAddressBuffer counter = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer source = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[P[0].y];
    const uint threads = min(counter.Load(P[1].w), P[0].w) * P[1].x;
    const uint count = threads / P[1].y + (threads % P[1].y != 0 ? 1u : 0u);
    if (command == 0) arguments.Store(0, count);
    if (command >= count || command >= P[1].z) return;
    const uint first = command * P[1].y;
    const uint record = 16 + command * P[2].y;
    arguments.Store(record, (first / (P[3].x ? P[3].x : P[3].z)) | P[3].y);
    for (uint at = 0; at < P[2].z; at += 4)
        arguments.Store(record + 4 + at, source.Load(P[2].x + at));
    const uint live = min(threads - first, P[1].y);
    arguments.Store3(record + 4 + P[2].w, P[3].x ? uint3(P[3].x, live / P[3].x, 1) : uint3(live, 1, 1));
}
