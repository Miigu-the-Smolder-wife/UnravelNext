// Compressed cluster vertices (visibility.cluster_compression): the decode, and the cluster vertex fetch that takes it
// when the cluster has a stream (Scene.hlsli includes this after loadVertex). Owner: V. CPU mirror and the format:
// Tools/ClusterBuilder ClusterStream.h / .cpp (streamVertex is this arithmetic).
// g_clusterStream (the SRV + 1 of raw words; 0: the scene has none): a header of STREAM_HEADER_WORDS (word 1: the
// clusters with a record), a record of STREAM_RECORD_WORDS per cluster, then the clusters' vertex bits.
// The decoded position is the mesh vertex's float (the cook puts the positions of a compressed mesh on its grid), so
// a reader of the stream and a reader of the 32 B vertex pool see the same triangle; normal, tangent and uv are
// quantised (the pool's are not).
// Who reads it: V's raster kernels (loadClusterVertex / deformClusterVertex, Deformation.hlsli). The readers that go
// from a vis id to the triangle (loadClusterTriangle, then loadVertex: the material resolve, motion, history) keep the
// pool: the decode inlined at each of their fetches put three kernels past the 200 KB limit (CoverageComposite,
// GiTrace, ReflectionTraceInline). loadClusterVertex is the fetch for them once those have room.
// Cost of a decode: the record (three loads, the same words for every vertex of the cluster) and a five-word window
// of the vertex's bits (two loads); a kernel that takes only the position leaves the rest undone.
#ifndef UNX_CLUSTER_STREAM_HLSLI
#define UNX_CLUSTER_STREAM_HLSLI

#define STREAM_HEADER_WORDS 4u
#define STREAM_RECORD_WORDS 10u

// 'count' bits (<= 24) at bit 'bit' of the window; advances 'bit'.
uint streamBits(uint window[5], inout uint bit, uint count)
{
    const uint word = bit >> 5, shift = bit & 31u;
    const uint lo = window[min(word, 4u)], hi = window[min(word + 1, 4u)];
    bit += count;
    const uint v = shift == 0 ? lo : (lo >> shift) | (hi << (32 - shift));
    return v & ((1u << count) - 1);  // (count 0: 0)
}

float streamUnit(uint q, uint bits) { return (float)q / (float)((1u << bits) - 1) * 2.0 - 1.0; }

// The tangent basis around a unit normal (clusterbuilder::streamBasis).
void streamBasis(float3 n, out float3 u, out float3 v)
{
    const float s = n.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z), b = n.x * n.y * a;
    u = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    v = float3(b, s + n.y * n.y * a, -n.y);
}

// Local vertex 'local' of the cluster whose record starts at byte 'record' (its dataOffset is not UNX_NONE).
VertexData loadStreamVertex(ByteAddressBuffer stream, uint record, uint local)
{
    const uint4 r0 = stream.Load4(record), r1 = stream.Load4(record + 16);
    const uint2 r2 = stream.Load2(record + 32);
    const float3 positionMin = asfloat(r0.xyz);
    const float positionStep = asfloat(r0.w);
    const uint dataOffset = r1.x, bits = r1.y;
    const float2 uvMin = asfloat(r1.zw), uvStep = asfloat(r2);
    const uint3 pb = uint3(bits & 31u, (bits >> 5) & 31u, (bits >> 10) & 31u);
    const uint nb = (bits >> 15) & 15u, tb = (bits >> 19) & 15u, ub = (bits >> 23) & 15u, vb = (bits >> 27) & 15u;
    const uint vertexBits = pb.x + pb.y + pb.z + 2 * nb + (tb != 0 ? tb + 1 : 0) + ub + vb;  // <= 129
    const uint first = local * vertexBits;
    const uint base = 4 * (dataOffset + (first >> 5));
    const uint4 w0 = stream.Load4(base);
    uint window[5] = { w0.x, w0.y, w0.z, w0.w, stream.Load(base + 16) };
    uint bit = first & 31u;
    VertexData v;
    const uint gx = streamBits(window, bit, pb.x);
    const uint gy = streamBits(window, bit, pb.y);
    const uint gz = streamBits(window, bit, pb.z);
    v.position = positionMin + float3(gx, gy, gz) * positionStep;  // (exact: grid coordinates x a power of two)
    const uint qx = streamBits(window, bit, nb);
    const uint qy = streamBits(window, bit, nb);
    const float2 e = float2(streamUnit(qx, nb), streamUnit(qy, nb));
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    v.normal = normalize(n);
    float3 tu, tv;
    streamBasis(v.normal, tu, tv);
    v.tangent = tu;
    v.tangentSign = 1.0;
    if (tb != 0)
    {
        const float angle = (float)streamBits(window, bit, tb) * (6.28318530717958647692 / (float)(1u << tb));
        v.tangent = tu * cos(angle) + tv * sin(angle);
        v.tangentSign = streamBits(window, bit, 1) != 0 ? -1.0 : 1.0;
    }
    const uint qu = streamBits(window, bit, ub);
    const uint qv = streamBits(window, bit, vb);
    v.uv = uvMin + float2(qu, qv) * uvStep;
    return v;
}

// The byte of cluster 'cluster's record when it has a stream, UNX_NONE when it has none (the scene is not compressed,
// the cluster is a run-time mesh's or of a mesh that is skinned, morphed or off its grid).
uint clusterStreamRecord(uint cluster)
{
    if (g_clusterStream == 0) return UNX_NONE;
    ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
    if (cluster >= stream.Load(4)) return UNX_NONE;
    const uint record = 4 * (STREAM_HEADER_WORDS + STREAM_RECORD_WORDS * cluster);
    return stream.Load(record + 16) != UNX_NONE ? record : UNX_NONE;
}

// Local vertex 'local' of a cluster (cl = loadCluster(cluster), record = clusterStreamRecord(cluster): once per cluster):
// from the cluster's stream when it has one, else the mesh vertex its entry of the cluster vertex pool names.
// meshVertex: that mesh vertex, UNX_NONE for a stream vertex (its mesh has no skin and no morph: nothing reads
// vertices by mesh index for it).
VertexData loadClusterVertex(GpuMesh mesh, GpuCluster cl, uint record, uint local, out uint meshVertex)
{
    if (record != UNX_NONE)
    {
        ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
        meshVertex = UNX_NONE;
        return loadStreamVertex(stream, record, local);
    }
    StructuredBuffer<uint> verts = ResourceDescriptorHeap[g_clusterVertexIndices];
    meshVertex = verts[cl.vertexOffset + local];
    return loadVertex(mesh, meshVertex);
}

#endif
