// unx-kernel: cs_6_6 main
// r.card.frame.sources (CardLayout.hlsli, the card frame's words 22..27): one thread. The final gather names this
// frame's sources of indirect light for hits without cards (Passes/GI/LumenHitIndirect.hlsli) once they stand - this
// frame's translucency volume and radiance cache in place of the previous frame's volume the frame was written with.
// The readers recorded after it (the screen probes' rays, the reflections) take them from the frame.
// P[0] = { card frame UAV (raw), 0, 0, 0 }
// P[1] = { translucency volume parameters SRV (0xFFFFFFFF: none), radiance cache parameters SRV (0xFFFFFFFF: none),
//          the cache's indirection SRV, the cache's irradiance atlas SRV }
// P[2] = { the cache's depth atlas SRV, the cache's hit-mark list UAV (raw; 0xFFFFFFFF: none), 0, 0 }
#include "Passes/SurfaceCache/CardLayout.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer frame = ResourceDescriptorHeap[P[0].x];
    frame.Store4(MC_FRAME_SOURCES, P[1]);
    frame.Store2(MC_FRAME_SOURCES + 16, P[2].xy);
}
