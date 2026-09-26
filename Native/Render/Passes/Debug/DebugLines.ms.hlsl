// unx-kernel: ms_6_6 main
// Debug lines (E, A15): 32 lines per group, each a screen-space quad around its segment (half width + 1 px of margin
// for the coverage filter, the caps included: a zero-length line is a round dot). World segments are clipped to the
// near plane first; the quad's corners keep their end point's clip z and w, so z / w interpolates like the segment.
// Groups: ceil(lines / 32) (DebugArgs.hlsl). Root constants: DebugCommon.hlsli.
#include "Passes/Debug/DebugCommon.hlsli"

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices DebugVertex verts[128], out indices uint3 tris[64])
{
    ByteAddressBuffer buf = ResourceDescriptorHeap[P[0].x];
    const uint count = min(buf.Load(DEBUG_H_LINES), buf.Load(DEBUG_H_LINE_CAPACITY));
    const uint first = gid.x * DEBUG_PER_GROUP;
    const uint n = first < count ? min(DEBUG_PER_GROUP, count - first) : 0u;
    SetMeshOutputCounts(n * 4, n * 2);
    if (tid >= n) return;
    const uint o = DEBUG_HEADER_BYTES + (first + tid) * DEBUG_LINE_BYTES;
    const uint4 r0 = buf.Load4(o), r1 = buf.Load4(o + 16);
    const uint flags = r1.w >> 16;
    const float halfWidth = 0.5f * (r1.w & 0xFFFFu) / 256.0f;
    float4 ca = debugClip(asfloat(r0.xyz), flags), cb = debugClip(asfloat(r1.xyz), flags);
    const float wMin = (flags & DEBUG_SCREEN) ? 0.0f : g_nearPlane * 1.0001f;
    const bool live = ca.w >= wMin || cb.w >= wMin;
    if (ca.w < wMin) ca = lerp(ca, cb, (wMin - ca.w) / (cb.w - ca.w));
    else if (cb.w < wMin) cb = lerp(cb, ca, (wMin - cb.w) / (ca.w - cb.w));
    const float2 pa = debugPixelOf(ca), pb = debugPixelOf(cb);
    const float2 d = pb - pa;
    const float len = length(d);
    const float2 dir = len > 1e-4f ? d / len : float2(1, 0), nrm = float2(-dir.y, dir.x);
    const float e = halfWidth + 1.0f;
    const float2 corner[4] = { pa - dir * e - nrm * e, pa - dir * e + nrm * e, pb + dir * e - nrm * e, pb + dir * e + nrm * e };
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        DebugVertex v;
        if (live)
        {
            const float4 c = k < 2 ? ca : cb;
            v.position = debugClipOf(corner[k], c.z, c.w);
            v.uv = 0;
            v.segment = float4(pa, pb);
            v.extra = halfWidth;
            v.color = r0.w;
            v.info = flags;
        }
        else v = debugDegenerate();
        verts[tid * 4 + k] = v;
    }
    tris[tid * 2] = uint3(tid * 4, tid * 4 + 1, tid * 4 + 2);
    tris[tid * 2 + 1] = uint3(tid * 4 + 2, tid * 4 + 1, tid * 4 + 3);
}
