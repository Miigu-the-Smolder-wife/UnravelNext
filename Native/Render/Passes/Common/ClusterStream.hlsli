// Compressed cluster vertices (visibility.cluster_compression): the decode, and the cluster vertex fetch that takes it
// when the cluster has a stream (Scene.hlsli includes this after loadVertex). Owner: V. CPU mirror and the format:
// Tools/ClusterBuilder ClusterStream.h / .cpp (streamVertex is this arithmetic).
// g_clusterStream (the SRV + 1 of raw words; 0: the scene has none): a header of STREAM_HEADER_WORDS (word 1: the
// clusters with a record), a record of STREAM_RECORD_WORDS per cluster, then the vertex bits of the clusters that are
// always resident. With visibility.cluster_streaming the other clusters' bits are in pages (the record's page): the
// frame's page table (StructuredBuffer<uint2>: the SRV of the pool buffer that holds the page, the page's first word
// there; x = UNX_NONE: not resident) says where - ClusterVertexSource.
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
#define STREAM_RECORD_WORDS 12u
#define STREAM_RECORD_DATA 16u          // byte of the record's dataOffset
#define STREAM_RECORD_PAGE 40u          // ... of its page (UNX_NONE: always resident)
#define STREAM_RECORD_REFINED_PAGE 44u  // ... of the page of the group the cluster was simplified from (UNX_NONE: none)

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

// Where a cluster's vertices come from: its stream (record: the byte of its record in g_clusterStream; data: the buffer
// its bits are in and the first word of that buffer its dataOffset counts from) or, record UNX_NONE, the vertex pool.
struct ClusterVertexSource
{
    uint record, dataSrv, dataBase;
};

// The byte of cluster 'cluster's record, UNX_NONE when the scene has no stream or the cluster no record (a run-time
// mesh's cluster).
uint clusterStreamRecord(uint cluster)
{
    if (g_clusterStream == 0) return UNX_NONE;
    ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
    if (cluster >= stream.Load(4)) return UNX_NONE;
    return 4 * (STREAM_HEADER_WORDS + STREAM_RECORD_WORDS * cluster);
}

// The pages of a cluster: x its own group's, y that of the group it was simplified from (UNX_NONE: always resident / none).
uint2 clusterStreamPages(uint cluster)
{
    const uint record = clusterStreamRecord(cluster);
    if (record == UNX_NONE) return uint2(UNX_NONE, UNX_NONE);
    ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
    return stream.Load2(record + STREAM_RECORD_PAGE);
}

// pageTable: the frame's page table's SRV + 1 (0: no streaming this frame - a cluster whose bits are in a page then takes
// the vertex pool, as does one whose page is not resident: the cull does not draw such a cluster, a reader that still
// asks gets the same vertices).
ClusterVertexSource clusterVertexSource(uint cluster, uint pageTable)
{
    ClusterVertexSource s;
    s.record = UNX_NONE;
    s.dataSrv = 0;
    s.dataBase = 0;
    const uint record = clusterStreamRecord(cluster);
    if (record == UNX_NONE) return s;
    ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
    if (stream.Load(record + STREAM_RECORD_DATA) == UNX_NONE) return s;  // (not compressed: skinned, morphed, off its grid)
    const uint page = stream.Load(record + STREAM_RECORD_PAGE);
    if (page == UNX_NONE)
    {
        s.record = record;
        s.dataSrv = g_clusterStream - 1;
        return s;
    }
    if (pageTable == 0) return s;
    StructuredBuffer<uint2> table = ResourceDescriptorHeap[pageTable - 1];
    const uint2 at = table[page];
    if (at.x == UNX_NONE) return s;
    s.record = record;
    s.dataSrv = at.x;
    s.dataBase = at.y;
    return s;
}

// Local vertex 'local' of a cluster with a stream (s.record != UNX_NONE).
VertexData loadStreamVertex(ClusterVertexSource s, uint local)
{
    ByteAddressBuffer stream = ResourceDescriptorHeap[g_clusterStream - 1];
    ByteAddressBuffer data = ResourceDescriptorHeap[s.dataSrv];
    const uint4 r0 = stream.Load4(s.record), r1 = stream.Load4(s.record + 16);
    const uint2 r2 = stream.Load2(s.record + 32);
    const float3 positionMin = asfloat(r0.xyz);
    const float positionStep = asfloat(r0.w);
    const uint dataOffset = r1.x, bits = r1.y;
    const float2 uvMin = asfloat(r1.zw), uvStep = asfloat(r2);
    const uint3 pb = uint3(bits & 31u, (bits >> 5) & 31u, (bits >> 10) & 31u);
    const uint nb = (bits >> 15) & 15u, tb = (bits >> 19) & 15u, ub = (bits >> 23) & 15u, vb = (bits >> 27) & 15u;
    const uint vertexBits = pb.x + pb.y + pb.z + 2 * nb + (tb != 0 ? tb + 1 : 0) + ub + vb;  // <= 129
    const uint first = local * vertexBits;
    const uint base = 4 * (s.dataBase + dataOffset + (first >> 5));
    const uint4 w0 = data.Load4(base);
    uint window[5] = { w0.x, w0.y, w0.z, w0.w, data.Load(base + 16) };
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

// Local vertex 'local' of a cluster (cl = loadCluster(cluster), source = clusterVertexSource(cluster, ...): once per
// cluster): from the cluster's stream when it has one, else the mesh vertex its entry of the cluster vertex pool names.
// meshVertex: that mesh vertex, UNX_NONE for a stream vertex (its mesh has no skin and no morph: nothing reads
// vertices by mesh index for it).
VertexData loadClusterVertex(GpuMesh mesh, GpuCluster cl, ClusterVertexSource source, uint local, out uint meshVertex)
{
    if (source.record != UNX_NONE)
    {
        meshVertex = UNX_NONE;
        return loadStreamVertex(source, local);
    }
    StructuredBuffer<uint> verts = ResourceDescriptorHeap[g_clusterVertexIndices];
    meshVertex = verts[cl.vertexOffset + local];
    return loadVertex(mesh, meshVertex);
}

#endif
