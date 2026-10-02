// unx-kernel: ms_6_6 main
// Diaphragm depth of field, the scattered highlights' quads (DdofScatter.hlsli; the reference's
// DOFHybridScatterVertexShader.usf): 32 records per group, each a quad around its 2 x 2 pixels wide enough for the
// widest of the four discs (the radius to the shape's circumscribed circle - x / squeeze across -, half a pixel to the
// block's centre and half a pixel of the edge's feather). Groups: ceil(capacity / 32); a group past the records
// appended emits nothing. Per record, once for its four vertices: the Petzval matrix of the block and what the
// barrel's and the matte box's cut needs (the reference computes those per pixel of the block in its reduce and stores
// them in the list; here the list keeps its record and the mesh kernel has the same inputs).
#include "Passes/Shading/DdofScatter.hlsli"

[outputtopology("triangle")]
[numthreads(DDOF_SPRITES_PER_GROUP, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices DdofSprite verts[128], out indices uint3 tris[64])
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    const uint count = min(list.Load(0), P[0].y);
    const uint first = gid.x * DDOF_SPRITES_PER_GROUP;
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
    const float2 size = float2(P[1].xy);
    const float squeeze = asfloat(P[2].x);
    const float2 extent = widest * asfloat(P[1].z) * float2(rcp(squeeze), 1.0) + 1.0;

    // the block's centre across the picture, -1 .. 1 (y as the pixels')
    const float2 at = (centre + 0.5) / size * 2.0 - 1.0;
    const float4 petzval = ddofPetzval(at, asfloat(P[4]), asfloat(P[5].xy), false);

    float2 bundle = 0, flag2 = 0;
    float4 spread = 0, flag01 = 0;
    const float barrelLength = asfloat(P[2].z);
    if (barrelLength >= 0.0)
    {
        // the direction of the block's point of the scene, as slopes over the axis (y up)
        const float2 slope = float2(at.x, -at.y) * asfloat(P[3].xy);
        bundle = slope * barrelLength;
        const float turn = (float)(int)P[1].w, focus = asfloat(P[3].z), infinity = max(asfloat(P[3].w), 1e-6);
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            // the point's distance from its radius: 1 / z = (1 - radius / radius at infinity) / focus
            const float perDistance = max(1.0 - turn * sprite[j].w / infinity, 0.0) / focus;
            // (ours: the bundle narrows over the barrel's length; the reference's expression has its radius there)
            spread[j] = turn * asfloat(P[2].w) * saturate(1.0 - barrelLength * perDistance) / max(sprite[j].w, 1e-3);
        }
        float2 edge[DDOF_MATTE_BOX_FLAGS];
        [unroll] for (uint f = 0; f < DDOF_MATTE_BOX_FLAGS; ++f)
        {
            const float4 flag = asfloat(P[6 + f]);
            edge[f] = flag.xy * flag.z - slope * flag.w;
        }
        flag01 = float4(edge[0], edge[1]);
        flag2 = edge[2];
    }

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
        v.petzval = petzval;
        v.bundle = bundle;
        v.spread = spread;
        v.flag01 = flag01;
        v.flag2 = flag2;
        verts[tid * 4 + k] = v;
    }
    tris[tid * 2] = uint3(tid * 4, tid * 4 + 1, tid * 4 + 2);
    tris[tid * 2 + 1] = uint3(tid * 4 + 2, tid * 4 + 1, tid * 4 + 3);
}
