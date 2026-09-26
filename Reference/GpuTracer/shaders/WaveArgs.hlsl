// unx-kernel: cs_6_6 main
// The next stage dispatch of the wavefront (Wave.hlsli): its dispatch arguments from the stage's appended count, which
// becomes the running count while the append count restarts at 0 for the stages after it. One thread.
// Root: x0 = wave buffer, y0 = arguments (RWByteAddressBuffer: thread groups x, y, z), sampleBegin = the stage.
#include "Common.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer wave = ResourceDescriptorHeap[g_root.x0];
    RWByteAddressBuffer args = ResourceDescriptorHeap[g_root.y0];
    const uint stage = g_root.sampleBegin;
    const uint n = wave.Load(stage * 4);
    wave.Store(16 + stage * 4, n);
    wave.Store(stage * 4, 0u);
    args.Store3(0, uint3((n + 63) / 64, 1, 1));
}
