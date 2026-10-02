// unx-kernel: ms_6_6 main
// Diaphragm depth of field, the scattered highlights' quads (DdofScatter.hlsli; the reference's
// DOFHybridScatterVertexShader.usf): 32 records per group, each a quad around its 2 x 2 pixels wide enough for the
// widest of the four discs (the radius to the shape's circumscribed circle, half a pixel to the block's centre and half a
// pixel of the edge's feather). Groups: ceil(capacity / 32); a group past the records appended emits nothing.
#include "Passes/Shading/DdofScatter.hlsli"

[outputtopology("triangle")]
[numthreads(DDOF_SPRITES_PER_GROUP, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices DdofSprite verts[128], out indices uint3 tris[64])
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    const uint count = min(list.Load(0), P[0].y);
    const uint first = (gid.x + gid.y * 65535u) * DDOF_SPRITES_PER_GROUP;  // (DiaphragmDof.cpp: rows of 65535 groups)
    const uint n = first < count ? min(DDOF_SPRITES_PER_GROUP, count - first) : 0u;
    SetMeshOutputCounts(n * 4, n * 2);
    if (tid >= n) return;
    const uint o = DDOF_SCATTER_HEADER + (first + tid) * DDOF_SCATTER_BYTES;
    const float2 centre = asfloat(list.Load2(o));
    float4 sprite[4];
    float widest = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        sprite[i] = asfloat(list.Load4(o + 16 + 16 * i));
        widest = max(widest, sprite[i].w);
    }
    const float extent = widest * asfloat(P[1].z) + 1.0;
    const float2 size = float2(P[1].xy);
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float2 corner = float2(k & 1, k >> 1) * 2.0 - 1.0;
        const float2 uv = (extent * corner + centre + 0.5) / size;
        DdofSprite v;
        v.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
        v.centre = centre;
        v.s0 = sprite[0];
        v.s1 = sprite[1];
        v.s2 = sprite[2];
        v.s3 = sprite[3];
        verts[tid * 4 + k] = v;
    }
    tris[tid * 2] = uint3(tid * 4, tid * 4 + 1, tid * 4 + 2);
    tris[tid * 2 + 1] = uint3(tid * 4 + 2, tid * 4 + 1, tid * 4 + 3);
}
