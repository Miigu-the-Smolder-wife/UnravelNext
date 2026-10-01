#include "unx/lights/EmissiveLights.h"

#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::lights
{
using namespace unx::render;

namespace
{
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct D2 { double x, y; };
struct Tri2 { D2 p[3]; float3 radiance; double luminance; };

// Area of a polygon (counter-clockwise positive).
double polygonArea(const std::vector<D2>& v)
{
    double a = 0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const D2& p = v[i];
        const D2& q = v[(i + 1) % v.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

// Sutherland-Hodgman clip of a convex polygon against the half-plane keep(point).
template <typename Keep, typename Cross>
void clipHalfPlane(std::vector<D2>& poly, std::vector<D2>& out, Keep keep, Cross cross)
{
    out.clear();
    if (poly.empty()) return;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const D2& a = poly[i];
        const D2& b = poly[(i + 1) % poly.size()];
        const bool ka = keep(a), kb = keep(b);
        if (ka) out.push_back(a);
        if (ka != kb) out.push_back(cross(a, b));
    }
    poly.swap(out);
}

// |triangle ∩ square| (exact).
double clippedArea(const Tri2& t, D2 centre, double half, std::vector<D2>& poly, std::vector<D2>& scratch)
{
    poly.assign(t.p, t.p + 3);
    if (polygonArea(poly) < 0) std::swap(poly[1], poly[2]);
    const double x0 = centre.x - half, x1 = centre.x + half, y0 = centre.y - half, y1 = centre.y + half;
    clipHalfPlane(poly, scratch, [&](D2 p) { return p.x >= x0; }, [&](D2 a, D2 b) { const double t2 = (x0 - a.x) / (b.x - a.x); return D2{ x0, a.y + (b.y - a.y) * t2 }; });
    clipHalfPlane(poly, scratch, [&](D2 p) { return p.x <= x1; }, [&](D2 a, D2 b) { const double t2 = (x1 - a.x) / (b.x - a.x); return D2{ x1, a.y + (b.y - a.y) * t2 }; });
    clipHalfPlane(poly, scratch, [&](D2 p) { return p.y >= y0; }, [&](D2 a, D2 b) { const double t2 = (y0 - a.y) / (b.y - a.y); return D2{ a.x + (b.x - a.x) * t2, y0 }; });
    clipHalfPlane(poly, scratch, [&](D2 p) { return p.y <= y1; }, [&](D2 a, D2 b) { const double t2 = (y1 - a.y) / (b.y - a.y); return D2{ a.x + (b.x - a.x) * t2, y1 }; });
    return poly.size() >= 3 ? std::max(0.0, polygonArea(poly)) : 0.0;
}

struct PlaneBuild
{
    float3 normal;
    double offset;  // normal . point
    float3 right, up;
    std::vector<Tri2> tris;
    double fluxIn = 0;
};

float3 anyPerpendicular(float3 n)
{
    const float3 a = std::abs(n.x) < 0.9f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
    return normalize(cross(n, a));
}

struct Builder
{
    const EmissiveCookConfig& cfg;
    EmissiveCook& out;
    std::vector<D2> poly, scratch;
    std::vector<uint32_t> indexScratch;

    // Node of square (centre, half) over the candidate triangles; returns its index or kNone when empty.
    uint32_t build(uint32_t planeIndex, const PlaneBuild& pb, D2 centre, double half, const std::vector<uint32_t>& candidates, uint32_t depth, double& fluxOut)
    {
        // Content: exact clipped areas; the candidates for the children are those with any area here.
        std::vector<uint32_t> inside;
        double lum = 0, area = 0;
        double r = 0, g = 0, b = 0;
        for (uint32_t ti : candidates)
        {
            const Tri2& t = pb.tris[ti];
            // quick reject: triangle bounds against the square
            double minX = std::min({ t.p[0].x, t.p[1].x, t.p[2].x }), maxX = std::max({ t.p[0].x, t.p[1].x, t.p[2].x });
            double minY = std::min({ t.p[0].y, t.p[1].y, t.p[2].y }), maxY = std::max({ t.p[0].y, t.p[1].y, t.p[2].y });
            if (maxX <= centre.x - half || minX >= centre.x + half || maxY <= centre.y - half || minY >= centre.y + half) continue;
            const double a = clippedArea(t, centre, half, poly, scratch);
            if (a <= 0) continue;
            inside.push_back(ti);
            area += a;
            r += a * t.radiance.x;
            g += a * t.radiance.y;
            b += a * t.radiance.z;
            lum += a * t.luminance;
        }
        if (inside.empty() || !(lum > 0)) return kNone;
        const double nodeArea = 4 * half * half;
        EmissiveNode n;
        n.centre = { (float)centre.x, (float)centre.y };
        n.half = (float)half;
        n.radiance = { (float)(r / nodeArea), (float)(g / nodeArea), (float)(b / nodeArea) };
        n.luminance = (float)(lum / nodeArea);
        n.plane = planeIndex;
        n.firstChild = kNone;
        const uint32_t index = (uint32_t)out.nodes.size();
        out.nodes.push_back(n);
        const bool leaf = 2 * half <= cfg.leafSize + 1e-9 || depth >= cfg.maxDepth;
        if (leaf)
        {
            fluxOut += lum;
            return index;
        }
        const uint32_t first = (uint32_t)out.nodes.size();
        out.nodes.resize(first + 4);  // reserve the 4 quadrant slots (children fill them; a missing quadrant stays kNone)
        for (uint32_t q = 0; q < 4; ++q)
        {
            EmissiveNode empty{};
            empty.firstChild = kNone;
            empty.plane = planeIndex;
            out.nodes[first + q] = empty;
        }
        out.nodes[index].firstChild = first;
        const double h = 0.5 * half;
        bool any = false;
        for (uint32_t q = 0; q < 4; ++q)
        {
            const D2 c{ centre.x + ((q & 1) ? h : -h), centre.y + ((q & 2) ? h : -h) };
            // Children are built into their slots: build() appends the child's subtree after the slots; the slot itself
            // holds the child's record (copied from the appended position), so the four quadrants stay consecutive.
            const uint32_t childIndex = build(planeIndex, pb, c, h, inside, depth + 1, fluxOut);
            if (childIndex == kNone) continue;
            any = true;
            // Move the child's record into its slot and redirect: the record appended at childIndex becomes a hole
            // marked with luminance 0 (readers never reach it: nothing points to it).
            out.nodes[first + q] = out.nodes[childIndex];
            out.nodes[childIndex].luminance = 0;
            out.nodes[childIndex].radiance = { 0, 0, 0 };
            out.nodes[childIndex].firstChild = kNone;
        }
        if (!any) out.nodes[index].firstChild = kNone;  // (cannot happen: the parent had content)
        return index;
    }
};
} // namespace

EmissiveCook cookEmissiveLights(const scene::Scene& scene, const EmissiveCookConfig& cfg)
{
    EmissiveCook out;
    out.convertedMaterials.assign((scene.materials.size() + 31) / 32, 0u);
    std::vector<PlaneBuild> planes;
    auto planeOf = [&](float3 n, double offset) -> PlaneBuild& {
        for (PlaneBuild& p : planes)
            if (dot(p.normal, n) > 1 - 1e-4 && std::abs(p.offset - offset) <= cfg.planeTolerance) return p;
        PlaneBuild p;
        p.normal = n;
        p.offset = offset;
        p.right = anyPerpendicular(n);
        p.up = cross(n, p.right);
        planes.push_back(p);
        return planes.back();
    };
    for (const scene::Instance& in : scene.instances)
    {
        if (in.mesh >= scene.meshes.size()) continue;
        const scene::Mesh& m = scene.meshes[in.mesh];
        for (size_t k = 0; k < m.submeshes.size(); ++k)
        {
            const scene::Submesh& sub = m.submeshes[k];
            const uint32_t mat = in.materialOverrides.empty() ? sub.material : in.materialOverrides[k];
            if (mat >= scene.materials.size()) continue;
            const scene::Material& material = scene.materials[mat];
            const float3 e = material.emissive;
            const double lum = 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z;
            if (!(lum > 0)) continue;
            const uint32_t triangles = sub.indexCount / 3;
            if (in.flags & scene::InstanceSkinned) { out.trianglesSkinned += triangles; continue; }
            if (material.emissiveTexture != scene::kNone) { out.trianglesTextured += triangles; continue; }
            out.convertedMaterials[mat / 32] |= 1u << (mat % 32);
            for (uint32_t t = 0; t < triangles; ++t)
            {
                const uint32_t i0 = m.indices[sub.indexOffset + 3 * t], i1 = m.indices[sub.indexOffset + 3 * t + 1], i2 = m.indices[sub.indexOffset + 3 * t + 2];
                const float3 p0 = in.transform.transformPoint(m.positions[i0]), p1 = in.transform.transformPoint(m.positions[i1]), p2 = in.transform.transformPoint(m.positions[i2]);
                const float3 c = cross(p1 - p0, p2 - p0);
                const float area2 = length(c);
                if (!(area2 > 1e-12f)) continue;
                const float3 n = c / area2;
                ++out.trianglesConverted;
                for (int side = 0; side < (material.twoSided ? 2 : 1); ++side)
                {
                    const float3 ns = side ? -n : n;
                    PlaneBuild& pb = planeOf(ns, (double)dot(ns, p0));
                    Tri2 tri;
                    const float3 pts[3] = { p0, p1, p2 };
                    for (int v = 0; v < 3; ++v) tri.p[v] = { (double)dot(pts[v], pb.right), (double)dot(pts[v], pb.up) };
                    tri.radiance = e;
                    tri.luminance = lum;
                    pb.tris.push_back(tri);
                    pb.fluxIn += 0.5 * area2 * lum;
                }
            }
        }
    }
    for (PlaneBuild& pb : planes)
    {
        if (pb.tris.empty()) continue;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        for (const Tri2& t : pb.tris)
            for (const D2& p : t.p)
            {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
        const D2 centre{ 0.5 * (minX + maxX), 0.5 * (minY + maxY) };
        const double half = 0.5 * std::max(maxX - minX, maxY - minY) * (1 + 1e-6) + 1e-6;
        EmissivePlane plane;
        plane.origin = pb.normal * (float)pb.offset + pb.right * (float)centre.x + pb.up * (float)centre.y;
        plane.right = pb.right;
        plane.up = pb.up;
        plane.normal = pb.normal;
        plane.flux = pb.fluxIn;
        const uint32_t planeIndex = (uint32_t)out.planes.size();
        // Triangles relative to the origin (the root square is centred there).
        for (Tri2& t : pb.tris)
            for (D2& p : t.p) { p.x -= centre.x; p.y -= centre.y; }
        std::vector<uint32_t> all(pb.tris.size());
        for (uint32_t i = 0; i < all.size(); ++i) all[i] = i;
        Builder b{ cfg, out, {}, {}, {} };
        double fluxOut = 0;
        plane.rootNode = b.build(planeIndex, pb, D2{ 0, 0 }, half, all, 0, fluxOut);
        out.fluxIn += pb.fluxIn;
        out.fluxOut += fluxOut;
        out.planes.push_back(plane);
    }
    return out;
}

std::vector<uint32_t> emissiveLightsImage(const EmissiveCook& cook)
{
    const uint32_t planeCount = (uint32_t)cook.planes.size(), nodeCount = (uint32_t)cook.nodes.size();
    const uint32_t planesOffset = 32, nodesOffset = planesOffset + planeCount * 64, bitsetOffset = nodesOffset + nodeCount * 48;
    std::vector<uint32_t> w((bitsetOffset + cook.convertedMaterials.size() * 4) / 4, 0u);
    auto putf = [&](uint32_t byte, float v) { std::memcpy(&w[byte / 4], &v, 4); };
    w[0] = planeCount;
    w[1] = nodeCount;
    w[2] = planesOffset;
    w[3] = nodesOffset;
    w[4] = bitsetOffset;
    w[5] = (uint32_t)cook.convertedMaterials.size();
    w[6] = 0;
    w[7] = 0;
    for (uint32_t i = 0; i < planeCount; ++i)
    {
        const EmissivePlane& p = cook.planes[i];
        const uint32_t o = planesOffset + i * 64;
        putf(o + 0, p.origin.x); putf(o + 4, p.origin.y); putf(o + 8, p.origin.z);
        putf(o + 16, p.right.x); putf(o + 20, p.right.y); putf(o + 24, p.right.z);
        putf(o + 32, p.up.x); putf(o + 36, p.up.y); putf(o + 40, p.up.z);
        putf(o + 48, p.normal.x); putf(o + 52, p.normal.y); putf(o + 56, p.normal.z);
        w[(o + 60) / 4] = p.rootNode;
    }
    for (uint32_t i = 0; i < nodeCount; ++i)
    {
        const EmissiveNode& n = cook.nodes[i];
        const uint32_t o = nodesOffset + i * 48;
        putf(o + 0, n.centre.x); putf(o + 4, n.centre.y); putf(o + 8, n.half); putf(o + 12, n.luminance);
        putf(o + 16, n.radiance.x); putf(o + 20, n.radiance.y); putf(o + 24, n.radiance.z);
        w[(o + 28) / 4] = n.plane;
        w[(o + 32) / 4] = n.firstChild;
        w[(o + 36) / 4] = 0; w[(o + 40) / 4] = 0; w[(o + 44) / 4] = 0;
    }
    std::copy(cook.convertedMaterials.begin(), cook.convertedMaterials.end(), w.begin() + bitsetOffset / 4);
    return w;
}

namespace
{
ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "emissive lights buffer");
    r->SetName(name);
    return r;
}

struct EmissiveLightsState
{
    uint64_t revision = 0;   // scene revision of the cook
    bool cooked = false;
    ComPtr<ID3D12Resource> buffer;
    std::vector<ComPtr<ID3D12Resource>> uploads;  // this frame's upload (released once its copy completed: the next call, or here)
    uint64_t bytes = 0;
    bool logged = false;
    Device* device = nullptr;  // for the deferred releases at destruction (a track state cleared while the GPU still copies:
                               // ShadingTests 2026-10-01 saw the debug layer's final-release corruption error)
    ~EmissiveLightsState()
    {
        if (!device) return;
        for (ComPtr<ID3D12Resource>& u : uploads) device->deferRelease(u);
        if (buffer) device->deferRelease(buffer);
    }
};
} // namespace

BufferRef emissiveLights(FramePassContext& fc)
{
    if (!fc.trackState) return {};
    if (!fc.quality.boolean("shading.emissive_area_lights")) return {};
    EmissiveLightsState& s = fc.state<EmissiveLightsState>("lights.emissive");
    Device& device = fc.device;
    s.device = &device;
    for (ComPtr<ID3D12Resource>& u : s.uploads) device.deferRelease(u);
    s.uploads.clear();
    RenderGraph& g = fc.graph;
    if (!s.cooked || s.revision != fc.scene.revision())
    {
        s.cooked = true;
        s.revision = fc.scene.revision();
        if (s.buffer) device.deferRelease(s.buffer);
        s.buffer.Reset();
        s.bytes = 0;
        const scene::Scene* src = fc.scene.source();
        if (!src) return {};
        EmissiveCookConfig cfg;
        cfg.leafSize = (float)fc.quality.number("shading.emissive_leaf_m");
        if (!(cfg.leafSize > 0)) fail("shading.emissive_leaf_m must be > 0");
        const EmissiveCook cook = cookEmissiveLights(*src, cfg);
        if (cook.planes.empty()) return {};
        const std::vector<uint32_t> image = emissiveLightsImage(cook);
        s.bytes = image.size() * 4;
        s.buffer = makeBuffer(device, s.bytes, D3D12_HEAP_TYPE_DEFAULT, L"M emissive quadtree lights");
        ComPtr<ID3D12Resource> upload = makeBuffer(device, s.bytes, D3D12_HEAP_TYPE_UPLOAD, L"M emissive quadtree lights upload");
        uint8_t* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(upload->Map(0, &none, reinterpret_cast<void**>(&p)), "map emissive lights upload");
        std::memcpy(p, image.data(), s.bytes);
        upload->Unmap(0, nullptr);
        const BufferRef table = g.importBuffer(s.buffer.Get(), BufferDesc{ "lights.emissive", s.bytes, 0 });
        ID3D12Resource* srcBuffer = upload.Get();
        const uint64_t bytes = s.bytes;
        g.addPass("lights.emissive.upload", QueueType::Graphics, [=](PassBuilder& b) { b.use(table, Use::CopyDst); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(table), 0, srcBuffer, 0, bytes); });
        s.uploads.push_back(upload);
        logf("M emissive area lights (14.1b): %zu planes, %zu nodes (%.1f MB), %u triangles converted (%u textured and %u skinned stay on the cache path); flux in %.6g out %.6g\n",
             cook.planes.size(), cook.nodes.size(), s.bytes / 1048576.0, cook.trianglesConverted, cook.trianglesTextured, cook.trianglesSkinned, cook.fluxIn, cook.fluxOut);
        return table;
    }
    if (!s.buffer) return {};
    return g.importBuffer(s.buffer.Get(), BufferDesc{ "lights.emissive", s.bytes, 0 });
}
} // namespace unx::lights
