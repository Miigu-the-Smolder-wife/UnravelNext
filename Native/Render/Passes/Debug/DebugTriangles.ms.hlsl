// unx-kernel: ms_6_6 main
// Debug triangles (E, A15): 32 per group, no culling; the rasterizer clips at the near plane.
#include "Passes/Debug/DebugCommon.hlsli"

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices DebugVertex verts[96], out indices uint3 tris[32])
{
    ByteAddressBuffer buf = ResourceDescriptorHeap[P[0].x];
    const uint lineCapacity = buf.Load(DEBUG_H_LINE_CAPACITY);
    const uint count = min(buf.Load(DEBUG_H_TRIANGLES), buf.Load(DEBUG_H_TRIANGLE_CAPACITY));
    const uint first = gid.x * DEBUG_PER_GROUP;
    const uint n = first < count ? min(DEBUG_PER_GROUP, count - first) : 0u;
    SetMeshOutputCounts(n * 3, n);
    if (tid >= n) return;
    const uint o = debugTrianglesOffset(lineCapacity) + (first + tid) * DEBUG_TRIANGLE_BYTES;
    const uint4 r[3] = { buf.Load4(o), buf.Load4(o + 16), buf.Load4(o + 32) };
    const uint flags = r[1].w;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        DebugVertex v = (DebugVertex)0;
        v.position = debugClip(asfloat(r[k].xyz), flags);
        v.color = r[0].w;
        v.info = flags;
        verts[tid * 3 + k] = v;
    }
    tris[tid] = uint3(tid * 3, tid * 3 + 1, tid * 3 + 2);
}
