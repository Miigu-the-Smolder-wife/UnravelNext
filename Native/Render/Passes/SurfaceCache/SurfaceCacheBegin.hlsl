// unx-kernel: cs_6_6 main
// The surface cache's frame start (SurfaceCache.hlsli): the header for this frame, one thread. With P[0].w != 0 (a new
// buffer, a scene revision) every thread clears a slot instead - keys and heads of the cells and of the probes - and the
// first writes the header.
// P[0] = { cache UAV, cells N, frame, clear }, P[1] = { max unused frames, flags (bit 0: marking on, bit 1: bilinear reads), asuint(radiosity ray
// cap, exposed units), asuint(probe max frames) }; frame constants b1 = main view (the camera the levels are taken from).
#include "Passes/SurfaceCache/SurfaceCache.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const uint n = P[0].y;
    if (P[0].w != 0)
    {
        if (id.x < n)
        {
            b.Store(scKeysOffset(id.x), 0u);
            b.Store(scHeadsOffset(n, id.x), 0u);
        }
        if (id.x < scProbeCount(n))
        {
            b.Store(scProbeKeysOffset(n, id.x), 0u);
            b.Store(scProbeHeadsOffset(n, id.x), 0u);
        }
    }
    if (id.x != 0) return;
    b.Store4(0, uint4(n, P[0].z, 0, 0));
    b.Store4(16, uint4(asuint(g_cameraPosition), P[1].x));
    b.Store4(32, uint4(0, 0, P[1].y, P[1].z));
    b.Store2(48, uint2(P[1].w, 0));  // (word 13: the feedback list's count)
}
