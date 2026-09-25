// unx-kernel: cs_6_6 main
// Core: per-frame GPU scene updates (GpuScene::flushUpdates): scatters 16-byte elements from the frame's upload slot
// into the scene buffers. Upload layout: count headers (target << 28 | destination element), padded to 16 B, then
// count uint4 payloads. Targets: 0 instances, 1 bone palette, 2 previous bone palette.
//   P[0] upload SRV (raw), element count, instances UAV (raw), bone palette UAV (raw); P[1].x previous palette UAV (raw)
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint count = P[0].y;
    if (i >= count) return;
    ByteAddressBuffer upload = ResourceDescriptorHeap[P[0].x];
    const uint header = upload.Load(4 * i);
    const uint4 payload = upload.Load4(((count * 4 + 15) & ~15u) + 16 * i);
    const uint target = header >> 28, element = header & 0x0FFFFFFFu;
    RWByteAddressBuffer destination = ResourceDescriptorHeap[target == 0 ? P[0].z : (target == 1 ? P[0].w : P[1].x)];
    destination.Store4(16 * element, payload);
}
