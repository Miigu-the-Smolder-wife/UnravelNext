// unx-kernel: ms_6_6 main
// Debug text (E, A15): 32 glyphs per group, each a pixel-aligned quad of its cell (height = size px, width = size x
// DEBUG_GLYPH_ASPECT) at the anchor's pixel + offset, at the anchor's depth. An anchor behind the near plane hides it.
#include "Passes/Debug/DebugCommon.hlsli"

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices DebugVertex verts[128], out indices uint3 tris[64])
{
    ByteAddressBuffer buf = ResourceDescriptorHeap[P[0].x];
    const uint count = min(buf.Load(DEBUG_H_GLYPHS), buf.Load(DEBUG_H_GLYPH_CAPACITY));
    const uint first = gid.x * DEBUG_PER_GROUP;
    const uint n = first < count ? min(DEBUG_PER_GROUP, count - first) : 0u;
    SetMeshOutputCounts(n * 4, n * 2);
    if (tid >= n) return;
    const uint o = debugGlyphsOffset(buf.Load(DEBUG_H_LINE_CAPACITY), buf.Load(DEBUG_H_TRIANGLE_CAPACITY)) + (first + tid) * DEBUG_GLYPH_BYTES;
    const uint4 r0 = buf.Load4(o), r1 = buf.Load4(o + 16);
    const uint flags = r1.z >> 16;
    const float size = (float)((r1.z >> 8) & 0xFFu);
    const float4 c = debugClip(asfloat(r0.xyz), flags);
    const bool live = (flags & DEBUG_SCREEN) || c.w >= g_nearPlane * 1.0001f;
    const float2 topLeft = round(debugPixelOf(c) + asfloat(r1.xy));  // whole pixels: the atlas cell maps onto pixel centres
    const float2 cell = float2(size * DEBUG_GLYPH_ASPECT, size);
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float2 uv = float2(k & 1, k >> 1);
        DebugVertex v = (DebugVertex)0;
        if (live)
        {
            v.position = debugClipOf(topLeft + uv * cell, c.z, c.w);
            v.uv = uv;
            v.extra = size;
            v.color = r0.w;
            v.info = (r1.z & 0xFFu) | (flags << 16);
        }
        else v = debugDegenerate();
        verts[tid * 4 + k] = v;
    }
    tris[tid * 2] = uint3(tid * 4, tid * 4 + 1, tid * 4 + 2);
    tris[tid * 2 + 1] = uint3(tid * 4 + 2, tid * 4 + 1, tid * 4 + 3);
}
