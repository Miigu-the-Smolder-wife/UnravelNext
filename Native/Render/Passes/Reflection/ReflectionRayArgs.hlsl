// unx-kernel: cs_6_6 main
// Indirect arguments of the split reflection passes (ReflectionRay.hlsli). Stage 0, after the trace: Dispatch arguments
// of the shade pass (one thread per allocated slot) and the combine pass (one thread per job). Stage 1, after the shade:
// the shadow pass's DispatchRays width (queued shadow rays) and the penumbra pass's Dispatch arguments (queued penumbra
// hits, ReflectionPenumbra). Stage 0 also sets the local-light shadow pass's width (one ray generation thread per
// allocated slot, ReflectionLocalShadow).
// P[0] = { arguments UAV (raw), rays UAV (raw), stage, penumbra args offset }, P[1] = { shade args offset, combine args offset, shadow
// description Width offset, local shadow description Width offset }, P[2] = { first inline description's Width offset,
// description stride, descriptions (ReflectionTraceInline: SKY x JOB): one thread per job each, inline bands },
// P[3] = { bytes of one band's arguments, threads of a band, bands of the slot and shadow passes, jobs of an inline band }
// (ReflectionRay.hlsli REFL_BAND: band b's ray dispatch descriptions sit b x P[3].x bytes on and get band b's share of
// the count; the compute passes' Dispatch arguments exist once).
#include "Bindless.hlsli"

uint bandWidth(uint count, uint band, uint bands, uint size)
{
    const uint first = min(band * size, count);
    return band + 1 == bands ? count - first : min(count - first, size);
}

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[0].y];
    const uint4 header = rays.Load4(0);  // allocated, capacity, shadow rays, jobs
    if (P[0].z == 0)
    {
        // Groups of 64 in two dimensions (at most 65535 per dimension): the kernels index group.y * 65535 + group.x.
        const uint shade = (min(header.x, header.y) + 63) / 64, combine = (header.w + 63) / 64;
        args.Store3(P[1].x, uint3(min(shade, 65535u), (shade + 65534) / 65535, 1));
        args.Store3(P[1].y, uint3(min(combine, 65535u), (combine + 65534) / 65535, 1));
        const uint slots = min(header.x, header.y), bands = max(P[3].z, 1u), inlineBands = max(P[2].w, 1u);
        [loop] for (uint b = 0; b < bands; ++b) args.Store(b * P[3].x + P[1].w, bandWidth(slots, b, bands, P[3].y));
        [loop] for (uint ib = 0; ib < inlineBands; ++ib)
            [loop] for (uint i = 0; i < P[2].z; ++i) args.Store(ib * P[3].x + P[2].x + i * P[2].y, bandWidth(header.w, ib, inlineBands, P[3].w));
    }
    else
    {
        const uint bands = max(P[3].z, 1u);
        [loop] for (uint b = 0; b < bands; ++b) args.Store3(b * P[3].x + P[1].z, uint3(bandWidth(header.z, b, bands, P[3].y), 1, 1));
        const uint penumbra = (min(rays.Load(16), header.y) + 63) / 64;
        args.Store3(P[0].w, uint3(min(penumbra, 65535u), (penumbra + 65534) / 65535, 1));
    }
}
