#include "unx/clusterbuilder/ClusterStream.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

// The compressed cluster vertex stream (ClusterStream.h): the encoder, and the CPU mirror of the GPU decode.
namespace unx::clusterbuilder
{
namespace
{
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr int32_t kGridLimit = 1 << 23;       // |grid coordinate| below this: coordinate x step is an exact float
constexpr uint32_t kPositionBitsMax = 21;     // a cluster's extent along an axis, in steps

uint32_t bitsFor(uint32_t range)  // bits that hold 0 .. range
{
    uint32_t bits = 0;
    while (bits < 32 && (range >> bits) != 0) ++bits;
    return bits;
}

struct BitWriter
{
    std::vector<uint32_t>& words;
    size_t first;       // the cluster's first word
    uint64_t bit = 0;   // from it
    void put(uint32_t value, uint32_t count)
    {
        if (count == 0) return;
        const size_t word = first + (size_t)(bit >> 5);
        const uint32_t shift = (uint32_t)(bit & 31);
        if (words.size() < word + 2) words.resize(word + 2, 0);
        const uint32_t v = count >= 32 ? value : value & ((1u << count) - 1);
        words[word] |= v << shift;
        if (shift + count > 32) words[word + 1] |= v >> (32 - shift);
        bit += count;
    }
    void finish()  // (the last word written; nothing after it that the next cluster does not own)
    {
        const size_t end = first + (size_t)((bit + 31) >> 5);
        words.resize(std::max(end, first));
    }
};

// 'count' bits (<= 24) at bit 'bit' of the words from 'first': the GPU's streamBits.
uint32_t readBits(const uint32_t* words, size_t wordCount, size_t first, uint64_t& bit, uint32_t count)
{
    if (count == 0) return 0;
    const size_t word = first + (size_t)(bit >> 5);
    const uint32_t shift = (uint32_t)(bit & 31);
    const uint32_t lo = word < wordCount ? words[word] : 0, hi = word + 1 < wordCount ? words[word + 1] : 0;
    bit += count;
    const uint32_t v = shift == 0 ? lo : (lo >> shift) | (hi << (32 - shift));
    return v & ((1u << count) - 1);
}

void octahedral(float3 n, float& ex, float& ey)
{
    const float s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    ex = s > 0 ? n.x / s : 0;
    ey = s > 0 ? n.y / s : 0;
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(ey)) * (ex >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(ex)) * (ey >= 0 ? 1.f : -1.f);
        ex = ox;
        ey = oy;
    }
}

float3 octahedralDecode(float ex, float ey)
{
    float3 n{ ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(n.y)) * (n.x >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(n.x)) * (n.y >= 0 ? 1.f : -1.f);
        n.x = ox;
        n.y = oy;
    }
    return normalize(n);
}

uint32_t quantizeUnit(float v, uint32_t bits)  // [-1, 1] -> 0 .. 2^bits - 1
{
    const float top = (float)((1u << bits) - 1);
    return (uint32_t)std::lround(std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * top);
}
float unitOf(uint32_t q, uint32_t bits) { return (float)q / (float)((1u << bits) - 1) * 2 - 1; }

const render::ClusterData::Named* streamOf(const render::ClusterData& data)
{
    for (const render::ClusterData::Named& n : data.named)
        if (n.name == kClusterStream) return &n;
    return nullptr;
}
} // namespace

int32_t gridCoordinate(float p, float step) { return (int32_t)std::floor((double)p / (double)step + 0.5); }

bool onGrid(float3 p, float step)
{
    return (float)gridCoordinate(p.x, step) * step == p.x && (float)gridCoordinate(p.y, step) * step == p.y && (float)gridCoordinate(p.z, step) * step == p.z;
}

float positionStep(const std::vector<float3>& positions, const Settings& settings)
{
    float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    float reach = 0;
    for (const float3& p : positions)
    {
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        reach = std::max({ reach, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z) });
    }
    const float radius = positions.empty() ? 0.0f : 0.5f * length(hi - lo);
    float want = settings.positionStep > 0 ? settings.positionStep : 1.0f / 1024;
    if (radius > 0) want = std::min(want, radius / 4096);
    float step = std::exp2(std::floor(std::log2(std::max(want, 1e-12f))));
    while (reach / step >= (float)kGridLimit) step *= 2;
    return step;
}

bool streamMesh(const scene::Mesh& m)
{
    return !m.positions.empty() && m.skin.joints.empty() && m.blendShapes.empty() && !(m.vertexAnimation.framesPerSecond > 0) && m.normals.size() >= m.positions.size();
}

void snapPositions(scene::Scene& scene, const Settings& settings)
{
    for (scene::Mesh& m : scene.meshes)
    {
        if (!streamMesh(m)) continue;
        const float step = positionStep(m.positions, settings);
        for (float3& p : m.positions) p = { (float)gridCoordinate(p.x, step) * step, (float)gridCoordinate(p.y, step) * step, (float)gridCoordinate(p.z, step) * step };
    }
}

void streamBasis(float3 n, float3& u, float3& v)
{
    // (Duff et al., "Building an Orthonormal Basis, Revisited": no branch but the sign, no singular direction)
    const float s = n.z >= 0 ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z), b = n.x * n.y * a;
    u = { 1.0f + s * n.x * n.x * a, s * b, -s * n.x };
    v = { b, s + n.y * n.y * a, -n.y };
}

void encodeStream(const scene::Scene& scene, const Settings& settings, const LodVertices* lodVertices, const StreamGroups& groups, render::ClusterData& data,
                  StreamPages* pages)
{
    const uint32_t normalBits = std::clamp(settings.normalBits, 4u, 12u), tangentBits = std::clamp(settings.tangentBits, 4u, 11u);
    const uint32_t uvBits = std::clamp(settings.uvBits, 4u, 15u);
    const uint32_t none = render::gpu::kNone;
    const size_t clusterCount = data.clusters.size();
    std::vector<StreamRecord> records(clusterCount);
    for (StreamRecord& r : records) r.dataOffset = r.page = r.refinedPage = none;
    std::vector<std::vector<uint32_t>> bits(clusterCount);  // each compressed cluster's vertex bits
    uint64_t vertices = 0, vertexBitsTotal = 0, meshVertices = 0;
    uint32_t compressedMeshes = 0, offGrid = 0;
    for (size_t mi = 0; mi < scene.meshes.size() && mi < data.meshes.size(); ++mi)
    {
        const scene::Mesh& m = scene.meshes[mi];
        if (!streamMesh(m)) continue;
        // What the clusters index: the mesh's vertices, then the builder's own (their other streams are their source's).
        static const LodVertices::Mesh noLod;
        const LodVertices::Mesh& lod = lodVertices && mi < lodVertices->meshes.size() ? lodVertices->meshes[mi] : noLod;
        const size_t own = m.positions.size();
        auto position = [&](uint32_t v) { return v < own ? m.positions[v] : lod.positions[v - own]; };
        auto source = [&](uint32_t v) { return v < own ? v : lod.source[v - own]; };
        // The grid is the mesh's own positions' (the builder's vertices were made on it); every vertex must be on it for
        // the stream to give its float back, within the grid's reach.
        const float step = positionStep(m.positions, settings);
        bool exact = true;
        for (size_t v = 0; v < own + lod.positions.size() && exact; ++v)
        {
            const float3 q = position((uint32_t)v);
            exact = onGrid(q, step) && std::max({ std::fabs(q.x), std::fabs(q.y), std::fabs(q.z) }) / step < (float)kGridLimit;
        }
        if (!exact)
        {
            ++offGrid;
            continue;
        }
        const bool hasTangents = m.tangents.size() >= own, hasUv = m.uv0.size() >= own;
        const render::ClusterData::MeshRange& range = data.meshes[mi];
        ++compressedMeshes;
        meshVertices += own + lod.positions.size();
        for (uint32_t c = range.clusterOffset; c < range.clusterOffset + range.clusterCount; ++c)
        {
            const render::gpu::Cluster& cl = data.clusters[c];
            const uint32_t count = cl.counts & 0xFFu;
            if (count == 0) continue;
            const uint32_t* index = &data.clusterVertexIndices[cl.vertexOffset];
            int32_t lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX }, hi[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
            float2 uvLo{ FLT_MAX, FLT_MAX }, uvHi{ -FLT_MAX, -FLT_MAX };
            for (uint32_t i = 0; i < count; ++i)
            {
                const float3 p = position(index[i]);
                const int32_t g[3] = { gridCoordinate(p.x, step), gridCoordinate(p.y, step), gridCoordinate(p.z, step) };
                for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], g[a]), hi[a] = std::max(hi[a], g[a]);
                if (hasUv)
                {
                    const float2 uv = m.uv0[source(index[i])];
                    uvLo = { std::min(uvLo.x, uv.x), std::min(uvLo.y, uv.y) };
                    uvHi = { std::max(uvHi.x, uv.x), std::max(uvHi.y, uv.y) };
                }
            }
            // (a cluster wider than 2^21 steps - 2 km at a millimetre - keeps the vertex pool)
            if ((int64_t)hi[0] - lo[0] >= (int64_t)1 << kPositionBitsMax || (int64_t)hi[1] - lo[1] >= (int64_t)1 << kPositionBitsMax ||
                (int64_t)hi[2] - lo[2] >= (int64_t)1 << kPositionBitsMax)
                continue;
            StreamRecord& r = records[c];
            const uint32_t pb[3] = { bitsFor((uint32_t)(hi[0] - lo[0])), bitsFor((uint32_t)(hi[1] - lo[1])), bitsFor((uint32_t)(hi[2] - lo[2])) };
            r.positionMin = { (float)lo[0] * step, (float)lo[1] * step, (float)lo[2] * step };
            r.positionStep = step;
            const uint32_t tb = hasTangents ? tangentBits : 0;
            const uint32_t ub = hasUv && uvHi.x > uvLo.x ? uvBits : 0, vb = hasUv && uvHi.y > uvLo.y ? uvBits : 0;
            r.uvMin = hasUv ? uvLo : float2{ 0, 0 };
            r.uvStep = { ub ? (uvHi.x - uvLo.x) / (float)((1u << ub) - 1) : 0.0f, vb ? (uvHi.y - uvLo.y) / (float)((1u << vb) - 1) : 0.0f };
            r.bits = pb[0] | pb[1] << 5 | pb[2] << 10 | normalBits << 15 | tb << 19 | ub << 23 | vb << 27;
            r.dataOffset = 0;  // (compressed: placed below)
            BitWriter w{ bits[c], 0 };
            for (uint32_t i = 0; i < count; ++i)
            {
                const float3 p = position(index[i]);
                const uint32_t s = source(index[i]);
                w.put((uint32_t)(gridCoordinate(p.x, step) - lo[0]), pb[0]);
                w.put((uint32_t)(gridCoordinate(p.y, step) - lo[1]), pb[1]);
                w.put((uint32_t)(gridCoordinate(p.z, step) - lo[2]), pb[2]);
                float ex, ey;
                octahedral(m.normals[s], ex, ey);
                const uint32_t qx = quantizeUnit(ex, normalBits), qy = quantizeUnit(ey, normalBits);
                w.put(qx, normalBits);
                w.put(qy, normalBits);
                if (tb)
                {
                    // The angle of the tangent around the normal as the decode will have it.
                    const float3 n = octahedralDecode(unitOf(qx, normalBits), unitOf(qy, normalBits));
                    float3 u, v;
                    streamBasis(n, u, v);
                    const float3 t{ m.tangents[s].x, m.tangents[s].y, m.tangents[s].z };
                    const float angle = std::atan2(dot(t, v), dot(t, u));  // (-pi, pi]
                    const uint32_t steps = 1u << tb;
                    w.put((uint32_t)((int32_t)std::lround(angle / kTwoPi * (float)steps) & (int32_t)(steps - 1)), tb);
                    w.put(m.tangents[s].w < 0 ? 1u : 0u, 1);
                }
                if (ub) w.put((uint32_t)std::clamp<long>(std::lround((m.uv0[s].x - uvLo.x) / r.uvStep.x), 0, (long)((1u << ub) - 1)), ub);
                if (vb) w.put((uint32_t)std::clamp<long>(std::lround((m.uv0[s].y - uvLo.y) / r.uvStep.y), 0, (long)((1u << vb) - 1)), vb);
            }
            vertexBitsTotal += w.bit;
            w.finish();
            vertices += count;
        }
    }

    // Where each group's bits go: a page (whole groups, in order, a mesh's groups together) when a coarser group can
    // stand in for it - it is not terminal, all its clusters are compressed and it fits a page - else the buffer.
    const uint32_t pageWords = kStreamPageBytes / 4 - kStreamPadWords;
    std::vector<uint32_t> groupPage(groups.groups.size(), none);
    std::vector<std::vector<uint32_t>> pageData;
    std::vector<uint32_t> pageMesh;  // the mesh of each page (a page holds one mesh's groups)
    std::vector<uint32_t> clusterMesh(clusterCount, none);
    for (size_t mi = 0; mi < data.meshes.size(); ++mi)
        for (uint32_t c = data.meshes[mi].clusterOffset; c < data.meshes[mi].clusterOffset + data.meshes[mi].clusterCount && c < clusterCount; ++c) clusterMesh[c] = (uint32_t)mi;
    uint64_t pagedWords = 0;
    if (pages && groups.clusterGroup.size() == clusterCount)
        for (size_t g = 0; g < groups.groups.size(); ++g)
        {
            const StreamGroups::Group& group = groups.groups[g];
            if (group.terminal || group.clusterCount == 0) continue;
            size_t size = 0;
            bool whole = true;
            for (uint32_t c = group.firstCluster; c < group.firstCluster + group.clusterCount; ++c)
            {
                whole = whole && records[c].dataOffset != none;
                size += bits[c].size();
            }
            if (!whole || size == 0 || size > pageWords) continue;
            const uint32_t mesh = clusterMesh[group.firstCluster];
            if (pageData.empty() || pageMesh.back() != mesh || pageData.back().size() + size > pageWords)
            {
                pageData.emplace_back();
                pageMesh.push_back(mesh);
            }
            groupPage[g] = (uint32_t)pageData.size() - 1;
            std::vector<uint32_t>& page = pageData.back();
            for (uint32_t c = group.firstCluster; c < group.firstCluster + group.clusterCount; ++c)
            {
                records[c].dataOffset = (uint32_t)page.size();
                page.insert(page.end(), bits[c].begin(), bits[c].end());
                pagedWords += bits[c].size();
            }
        }
    // The buffer: header, records, the bits that are always resident.
    std::vector<uint32_t> words(kStreamHeaderWords + kStreamRecordWords * clusterCount, 0);
    words[0] = kStreamMagic;
    words[1] = (uint32_t)clusterCount;
    words[2] = (uint32_t)pageData.size();
    const bool grouped = groups.clusterGroup.size() == clusterCount;
    for (uint32_t c = 0; c < clusterCount; ++c)
    {
        StreamRecord& r = records[c];
        if (grouped)
        {
            r.page = groupPage[groups.clusterGroup[c]];
            r.refinedPage = groups.clusterRefined[c] != none ? groupPage[groups.clusterRefined[c]] : none;
        }
        if (r.dataOffset == none || r.page != none) continue;
        r.dataOffset = (uint32_t)words.size();
        words.insert(words.end(), bits[c].begin(), bits[c].end());
    }
    words.resize(words.size() + kStreamPadWords, 0);
    std::memcpy(&words[kStreamHeaderWords], records.data(), records.size() * sizeof(StreamRecord));
    auto named = [&](const char* name, const std::vector<uint32_t>& w) {
        render::ClusterData::Named n;
        n.name = name;
        n.stride = 4;
        n.bytes.resize(w.size() * 4);
        std::memcpy(n.bytes.data(), w.data(), n.bytes.size());
        data.named.push_back(std::move(n));
    };
    named(kClusterStream, words);
    // (what the stream stands beside: those clusters' entries of the cluster vertex pool, 4 B each, and the meshes'
    // 32 B vertices - a mesh vertex is in several clusters, of its own level and of the coarser ones)
    logf("cluster builder: compressed %llu cluster vertices of %u meshes: %.1f bits a vertex, %.2f MB of bits (with the records and what is not paged: the "
         "buffer %.2f MB); the same clusters' vertex pool entries %.2f MB, the meshes' %llu vertices in the 32 B pool %.2f MB\n",
         (unsigned long long)vertices, compressedMeshes, vertices ? (double)vertexBitsTotal / (double)vertices : 0.0, (double)vertexBitsTotal / 8 / 1048576.0,
         (double)words.size() * 4 / 1048576.0, (double)vertices * 4 / 1048576.0, (unsigned long long)meshVertices, (double)meshVertices * 32 / 1048576.0);
    if (offGrid)
        logf("cluster builder: %u rigid meshes are not on their position grid (clusterbuilder::snapPositions before the build): left uncompressed\n", offGrid);
    if (!pages) return;

    // The pages and their dependencies: the page of a group needs the pages of the groups that hold the clusters
    // simplified from it (they are drawn in its place while it is absent; with it resident and one of them not, a
    // stand-in for that one would be drawn over it).
    pages->pages.assign(pageData.size(), {});
    pages->dependencies.assign(pageData.size(), {});
    for (size_t i = 0; i < pageData.size(); ++i)
    {
        pageData[i].resize(pageData[i].size() + kStreamPadWords, 0);
        pages->pages[i].resize(pageData[i].size() * 4);
        std::memcpy(pages->pages[i].data(), pageData[i].data(), pages->pages[i].size());
    }
    if (grouped)
        for (uint32_t c = 0; c < clusterCount; ++c)
        {
            const uint32_t finer = records[c].refinedPage, own = records[c].page;
            if (finer == none || own == none || own == finer) continue;
            std::vector<uint32_t>& d = pages->dependencies[finer];
            if (std::find(d.begin(), d.end(), own) == d.end()) d.push_back(own);
        }
    std::vector<uint32_t> deps{ (uint32_t)pageData.size() };
    uint32_t offset = 0;
    for (const std::vector<uint32_t>& d : pages->dependencies)
    {
        deps.push_back(offset);
        offset += (uint32_t)d.size();
    }
    deps.push_back(offset);
    for (const std::vector<uint32_t>& d : pages->dependencies) deps.insert(deps.end(), d.begin(), d.end());
    named(kClusterPageDeps, deps);
    logf("cluster builder: %zu cluster pages (%.2f MB of the stream's bits; %.2f MB stay resident with the records)\n", pageData.size(),
         (double)pagedWords * 4 / 1048576.0, (double)words.size() * 4 / 1048576.0);
}

bool streamVertex(const render::ClusterData& data, uint32_t cluster, uint32_t local, StreamVertex& out, const StreamPages* pages)
{
    const render::ClusterData::Named* stream = streamOf(data);
    if (!stream || stream->bytes.size() < (size_t)kStreamHeaderWords * 4) return false;
    const uint32_t* buffer = reinterpret_cast<const uint32_t*>(stream->bytes.data());
    if (buffer[0] != kStreamMagic || cluster >= buffer[1]) return false;
    StreamRecord r;
    std::memcpy(&r, &buffer[kStreamHeaderWords + (size_t)kStreamRecordWords * cluster], sizeof r);
    if (r.dataOffset == render::gpu::kNone) return false;
    // The words the bits are in: the buffer, or the cluster's page.
    const uint32_t* words = buffer;
    size_t wordCount = stream->bytes.size() / 4;
    if (r.page != render::gpu::kNone)
    {
        if (!pages || r.page >= pages->pages.size()) return false;
        words = reinterpret_cast<const uint32_t*>(pages->pages[r.page].data());
        wordCount = pages->pages[r.page].size() / 4;
    }
    const uint32_t pb[3] = { r.bits & 31u, (r.bits >> 5) & 31u, (r.bits >> 10) & 31u };
    const uint32_t nb = (r.bits >> 15) & 15u, tb = (r.bits >> 19) & 15u, ub = (r.bits >> 23) & 15u, vb = (r.bits >> 27) & 15u;
    const uint32_t vertexBits = pb[0] + pb[1] + pb[2] + 2 * nb + (tb ? tb + 1 : 0) + ub + vb;
    uint64_t bit = (uint64_t)local * vertexBits;
    const uint32_t gx = readBits(words, wordCount, r.dataOffset, bit, pb[0]), gy = readBits(words, wordCount, r.dataOffset, bit, pb[1]);
    const uint32_t gz = readBits(words, wordCount, r.dataOffset, bit, pb[2]);
    out.position = { r.positionMin.x + (float)gx * r.positionStep, r.positionMin.y + (float)gy * r.positionStep, r.positionMin.z + (float)gz * r.positionStep };
    const uint32_t qx = readBits(words, wordCount, r.dataOffset, bit, nb), qy = readBits(words, wordCount, r.dataOffset, bit, nb);
    out.normal = octahedralDecode(unitOf(qx, nb), unitOf(qy, nb));
    float3 u, v;
    streamBasis(out.normal, u, v);
    out.tangent = u;
    out.tangentSign = 1;
    if (tb)
    {
        const float angle = (float)readBits(words, wordCount, r.dataOffset, bit, tb) * (kTwoPi / (float)(1u << tb));
        out.tangent = u * std::cos(angle) + v * std::sin(angle);
        out.tangentSign = readBits(words, wordCount, r.dataOffset, bit, 1) ? -1.0f : 1.0f;
    }
    out.uv = { r.uvMin.x + (float)readBits(words, wordCount, r.dataOffset, bit, ub) * r.uvStep.x, r.uvMin.y };
    out.uv.y = r.uvMin.y + (float)readBits(words, wordCount, r.dataOffset, bit, vb) * r.uvStep.y;
    return true;
}
} // namespace unx::clusterbuilder
