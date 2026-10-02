// unx-kernel: cs_6_6 main
// r.refl.lumen.args (reflection.lumen_only): this frame's job count (the classification's counter, word 0 of the
// arguments buffer) into the indirect DispatchRays descriptions of the Lumen trace (Width; Height = Depth = 1), held once
// per band: band b's descriptions get the jobs of [b x P[1].y, (b + 1) x P[1].y) - the last band the rest. A dispatch
// launches at most P[1].y threads, whatever the frame's count.
// P[0] = { arguments UAV (raw), descriptions a band, description stride, the first description's Width offset in a band }
// P[1] = { bytes a band, jobs a band, bands, 0 }
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    const uint jobs = args.Load(0);
    const uint bands = max(P[1].z, 1u);
    for (uint b = 0; b < bands; ++b)
    {
        const uint first = min(b * P[1].y, jobs);
        const uint width = b + 1 == bands ? jobs - first : min(jobs - first, P[1].y);
        for (uint i = 0; i < P[0].y; ++i) args.Store3(b * P[1].x + i * P[0].z + P[0].w, uint3(width, 1, 1));
    }
}
