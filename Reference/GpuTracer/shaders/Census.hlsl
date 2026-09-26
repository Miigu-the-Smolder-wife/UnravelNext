// unx-kernel: cs_6_6 main
// Primary-ray identities (diagnostics; the CPU's PathTracer::primaryIdentities, used by unx_reference probe): one thread
// per (pixel of a rectangle, sub-sample), sub-samples 0..15 the 4 x 4 stratum centres and 16 the pixel centre, the
// camera ray and near-plane t of the path kernel, the first hit of every surface (mask all). Output per thread: uint2
// (mesh triangle index, instance), 0xFFFFFFFF twice for the sky. One trace per thread: a dispatch is bounded by its
// thread count (the host sends at most 2^19).
// Root: constants, x0 = output UAV (RWByteAddressBuffer, 8 bytes per thread), y0 = first row, w = columns, h = rows,
// sampleBegin = first column, pathBase = first thread of this dispatch, pathCount = threads of this dispatch.
#include "Common.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_root.pathCount) return;
    const uint t = g_root.pathBase + id.x;
    const RtConstants C = rtC();
    const uint s = t % 17, p = t / 17;
    const uint x = g_root.sampleBegin + p % g_root.w, y = g_root.y0 + p / g_root.w;
    const float jx = s < 16 ? ((float)(s & 3) + 0.5f) / 4.0f : 0.5f, jy = s < 16 ? ((float)(s >> 2) + 0.5f) / 4.0f : 0.5f;
    const float3 dir = rtCameraRay(C.camera, C.width, C.height, (float)x + jx, (float)y + jy);
    const float tn = C.camera.nearPlane / max(dot(dir, C.camera.forward), 1e-3f);
    RtHit hit;
    uint2 o = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    if (rtIntersect(C.camera.position, dir, tn, kRtFarT, kRtMaskAll, hit))
    {
        uint first;
        rtSubmeshOf(rtInstance(hit.instance), hit.geometry, first);
        o = uint2(first + hit.primitive, hit.instance);
    }
    RWByteAddressBuffer out_ = ResourceDescriptorHeap[g_root.x0];
    out_.Store2(t * 8, o);
}
