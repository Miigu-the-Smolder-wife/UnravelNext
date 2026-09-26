// unx-kernel: cs_6_6 main
// The wavefront's next dispatch (PathTrace.hlsl): the dispatch arguments for the list the last dispatch wrote, and the
// other list's count reset for the dispatch after it. One thread.
// Root: x0 = wave buffer (RWByteAddressBuffer: [0] count of list 0, [4] count of list 1), y0 = arguments
// (RWByteAddressBuffer: thread groups x, y, z), sampleBegin = the list to dispatch (0 or 1).
#include "Common.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer wave = ResourceDescriptorHeap[g_root.x0];
    RWByteAddressBuffer args = ResourceDescriptorHeap[g_root.y0];
    const uint which = g_root.sampleBegin;
    const uint n = wave.Load(which * 4);
    args.Store3(0, uint3((n + 63) / 64, 1, 1));
    wave.Store((1 - which) * 4, 0u);
}
