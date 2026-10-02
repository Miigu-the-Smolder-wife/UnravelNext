#pragma once
// Compressed cluster vertices (visibility.cluster_compression; the reference's NaniteEncode.cpp / NaniteDataDecode.ush).
// Owner: V. GPU mirror: Native/Render/Passes/Common/ClusterStream.hlsli (loadClusterVertex).
//
// A cluster of a rigid mesh (no skin, no blend shapes, no vertex animation) whose positions are on the mesh's grid
// stores its own vertices as a bit stream:
//   position   the mesh's grid coordinates (p / step, step a power of two per mesh: positionStep) less the cluster's
//              smallest, in as many bits per axis as the cluster's extent needs (<= 21). The decoded position is the
//              coordinate x step - the mesh vertex's own float (both exact: |coordinate| < 2^23) - so a reader of the
//              stream and a reader of the 32 B vertex pool (the material resolve, the ray scene) see the same triangle,
//              and a vertex shared by clusters has one position in all of them.
//   normal     octahedral, normalBits per axis.
//   tangent    the angle around the decoded normal in tangentBits (basis: streamBasis), then the bitangent sign's bit;
//              a mesh without tangents stores none (the decode gives the basis's first axis).
//   uv         less the cluster's smallest, in steps of its range / (2^uvBits - 1) per axis (no bits for an axis that
//              does not vary).
// The triangles stay ClusterData::clusterTriangles (three 8-bit local indices a word, with the cut-edge flags), and the
// cluster vertex pool keeps the mesh vertex indices (the readers that go through them are unchanged).
//
// The grid: the cook puts a mesh's positions on it before the build (snapPositions: at most half a step per axis, a
// step being at most positionStep and a 4096th of the mesh's size); a mesh that is not on its grid is not compressed
// (the stream could not give its floats back). The builder's own vertices (LodVertices) are made on the grid.
//
// Buffer "clusterStream" (ClusterData::named, raw words): kStreamHeaderWords words { kStreamMagic, records, pages, 0 },
// then one StreamRecord per cluster (global index), then the vertex bits of the clusters that are always resident (each
// cluster at a word, vertex i at bit i x vertexBits) and four words of padding (the decode loads a five-word window).
//
// Pages (visibility.cluster_streaming; the reference's streaming pages, NaniteStreamingManager): with StreamPages given
// to the build, the vertex bits of every group that a coarser one can stand in for go into pages of at most
// kStreamPageBytes (whole groups, a mesh's groups in order) instead of the buffer; the buffer keeps the records and the
// bits of the terminal groups (each mesh's coarsest clusters: always resident). A record names its cluster's page and
// the page of the group it was simplified from: the cull draws a group only when its page is resident, and a cluster
// whose finer group is not resident in its place (CullNodes, CullClusters). A page depends on the pages of the groups
// that hold the clusters simplified from its groups (StreamPages::dependencies; "clusterPageDeps"): the runtime keeps a
// page resident only with them, so no group is drawn together with a stand-in for it.
//
// What it changes: V's vis buffer and depth rasters read the stream (about 10 B a vertex at the defaults, the cluster's
// vertices in a row) instead of an index and the 32 B vertex, and a cluster's vertices are self-contained (a streaming
// page's payload, Passes/Streaming). It does not shrink the scene: the 32 B vertex pool stays for the material
// resolve, the ray scene, the reference and every skinned instance.
#include "unx/clusterbuilder/ClusterBuilder.h"

namespace unx::clusterbuilder
{
constexpr const char* kClusterStream = "clusterStream";
constexpr uint32_t kStreamMagic = 0x31534C43u;  // "CLS1"
constexpr const char* kClusterPageDeps = "clusterPageDeps";  // raw words: pages, pages + 1 list offsets, the lists (page ids)
constexpr uint32_t kStreamHeaderWords = 4, kStreamRecordWords = 12, kStreamPadWords = 4;
constexpr uint32_t kStreamPageBytes = 64 * 1024;  // a page fits a slot of the streaming pool (streaming::Settings::slotBytes)

struct StreamRecord  // 48 B
{
    float3 positionMin;   // the cluster's smallest grid coordinate x step (object space)
    float positionStep;   // the mesh's grid step (m, a power of two)
    uint32_t dataOffset;  // first word of the cluster's vertex bits - in the buffer, or in its page; kNone: the cluster is
                          // not compressed
    uint32_t bits;        // x | y << 5 | z << 10 (5 bits each) | normal << 15 | tangent << 19 | u << 23 | v << 27 (4 bits each)
    float2 uvMin, uvStep;
    uint32_t page;        // the page that holds the cluster's group; kNone: always resident (the bits are in the buffer)
    uint32_t refinedPage; // the page of the group the cluster was simplified from; kNone: none, or always resident
};
static_assert(sizeof(StreamRecord) == kStreamRecordWords * 4);

// The groups of the hierarchy as the build put the clusters together (the clusters are in group order).
struct StreamGroups
{
    struct Group
    {
        uint32_t firstCluster = 0, clusterCount = 0;  // global cluster indices
        bool terminal = false;                         // no coarser clusters were made from it (the mesh's coarsest)
    };
    std::vector<Group> groups;
    std::vector<uint32_t> clusterGroup;    // per cluster: its group
    std::vector<uint32_t> clusterRefined;  // per cluster: the group it was simplified from (kNone: source geometry)
};

// The streamed part of the stream (build's last argument): the page file's payload (streaming::PageFileWriter::write) and
// each page's dependencies.
struct StreamPages
{
    std::vector<std::vector<uint8_t>> pages;
    std::vector<std::vector<uint32_t>> dependencies;
};

struct StreamVertex
{
    float3 position, normal, tangent;
    float tangentSign = 1;  // +1 / -1
    float2 uv;
};

// The mesh's grid step: the largest power of two at most Settings::positionStep and a 4096th of the bounds' half
// diagonal, raised until every coordinate fits (|p / step| < 2^23) - a function of the mesh's positions and the
// setting only.
float positionStep(const std::vector<float3>& positions, const Settings& settings);
// A coordinate on the grid (round to nearest), and whether a position is on it (the coordinate x step is its float).
int32_t gridCoordinate(float p, float step);
bool onGrid(float3 p, float step);
// Puts every compressible mesh's positions on its grid: the cook calls it before the build and GpuScene::upload when
// Settings::compression is on (a mesh it left off the grid is drawn uncompressed).
void snapPositions(scene::Scene& scene, const Settings& settings);
// Whether the mesh's clusters can be compressed (rigid, with normals).
bool streamMesh(const scene::Mesh& mesh);
// Appends the stream to 'data' (build calls it with Settings::compression; lodVertices: the builder's own vertices,
// which the clusters index after the mesh's). pages: where the streamed groups' bits go (null: everything in the buffer).
void encodeStream(const scene::Scene& scene, const Settings& settings, const LodVertices* lodVertices, const StreamGroups& groups, render::ClusterData& data,
                  StreamPages* pages);
// The CPU decode of a cluster's local vertex (the GPU's arithmetic); false when the cluster is not compressed, or its
// bits are in a page and 'pages' is not given.
bool streamVertex(const render::ClusterData& data, uint32_t cluster, uint32_t local, StreamVertex& out, const StreamPages* pages = nullptr);
// The decode's tangent basis around a unit normal (the GPU mirror: streamBasis, ClusterStream.hlsli).
void streamBasis(float3 n, float3& u, float3& v);
} // namespace unx::clusterbuilder
