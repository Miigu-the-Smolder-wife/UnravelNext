#include "unx/render/RenderGraph.h"

#include "unx/render/GpuProfiler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <unordered_map>

namespace unx::render
{
namespace
{
constexpr D3D12_BARRIER_SUBRESOURCE_RANGE kAllSubresources = { 0xffffffffu, 0, 0, 0, 0, 0 };

struct UseInfo
{
    bool write;
    D3D12_BARRIER_SYNC sync;
    D3D12_BARRIER_ACCESS access;
    D3D12_BARRIER_LAYOUT layout;
    bool disjoint = false;
};

UseInfo useInfo(Use u)
{
    switch (u)
    {
    case Use::SrvCompute: return { false, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE };
    case Use::SrvGraphics: return { false, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE };
    case Use::UavCompute: return { true, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS };
    case Use::UavComputeDisjoint: return { true, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, true };
    case Use::UavGraphics: return { true, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS };
    case Use::RenderTarget: return { true, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET };
    case Use::DepthWrite: return { true, D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE };
    case Use::DepthRead: return { false, D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ };
    case Use::IndirectArgs: return { false, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT, D3D12_BARRIER_LAYOUT_GENERIC_READ };
    case Use::CopySrc: return { false, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE };
    case Use::CopyDst: return { true, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST };
    }
    return {};
}

// Layouts only the direct queue may hold or transition.
bool directOnlyLayout(D3D12_BARRIER_LAYOUT l)
{
    return l == D3D12_BARRIER_LAYOUT_RENDER_TARGET || l == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE || l == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ ||
           l == D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE || l == D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
}

DXGI_FORMAT typelessDepth(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return f;
    }
}

DXGI_FORMAT depthSrvFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
    }
}

bool isDepthFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// NO_ACCESS is its own bit and may not be combined with other access bits.
D3D12_BARRIER_ACCESS orAccess(D3D12_BARRIER_ACCESS a, D3D12_BARRIER_ACCESS b)
{
    if (a == D3D12_BARRIER_ACCESS_NO_ACCESS) return b;
    if (b == D3D12_BARRIER_ACCESS_NO_ACCESS) return a;
    return a | b;
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

struct RenderGraph::Impl
{
    struct ResourceNode
    {
        bool texture = true;
        bool imported = false;
        TextureDesc tdesc;
        BufferDesc bdesc;
        std::string name;
        ID3D12Resource* importedResource = nullptr;
        D3D12_BARRIER_LAYOUT importLayout = D3D12_BARRIER_LAYOUT_COMMON;
    };
    struct UseRecord
    {
        uint32_t resource;
        Use use;
    };
    struct PassNode
    {
        std::string name;
        QueueType queue;
        ExecuteFn execute;
        std::vector<UseRecord> uses;
        bool keep = false;
    };

    // Merged use of one resource by one pass.
    struct Access
    {
        uint32_t resource;
        bool write;
        D3D12_BARRIER_SYNC sync;
        D3D12_BARRIER_ACCESS access;
        D3D12_BARRIER_LAYOUT layout;
        bool disjoint = false;
    };

    struct Views
    {
        uint32_t srv = UINT32_MAX, uav = UINT32_MAX, rtv = UINT32_MAX, dsv = UINT32_MAX, dsvRead = UINT32_MAX;
    };
    struct Physical
    {
        ComPtr<ID3D12Resource> resource;  // transient: owned placed resource
        uint64_t offset = 0, size = 0;
        Views views;
        uint64_t descKey = 0;
    };

    struct Barriers
    {
        std::vector<D3D12_TEXTURE_BARRIER> textures;
        std::vector<uint32_t> textureResources;  // resource ids, patched with the frame's pointers
        std::vector<D3D12_BUFFER_BARRIER> buffers;
        std::vector<uint32_t> bufferResources;
        bool empty() const { return textures.empty() && buffers.empty(); }
    };
    struct PlanPass
    {
        uint32_t pass;
        Barriers before, after;
    };
    struct Segment
    {
        QueueType queue;
        std::vector<PlanPass> passes;
        std::vector<uint32_t> waitSegments;  // indices of segments on other queues that must complete first
        bool firstOfQueue = false, lastOfQueue = false;
    };
    struct Plan
    {
        uint64_t key = 0;
        std::vector<Segment> segments;
        std::vector<Physical> physical;  // per resource id (imported entries unused)
        std::vector<bool> live;
        RenderGraphStats stats;
    };

    Device& device;
    std::vector<ResourceNode> resources;
    std::vector<PassNode> passes;
    std::unique_ptr<Plan> plan;
    ComPtr<ID3D12Heap> heap;
    uint64_t heapSize = 0;
    std::unordered_map<ID3D12Resource*, Views> importedViews;
    std::vector<ID3D12Resource*> framePointers;  // per resource id, this frame
    uint64_t prevFrameFence[kQueueTypeCount] = {};

    explicit Impl(Device& d) : device(d) {}

    ~Impl()
    {
        if (plan) releasePlan(*plan);
        for (auto& [ptr, v] : importedViews) releaseViews(v);
    }

    // Descriptor slots may still be read by frames in flight, so they return to the heap only after every queue
    // has passed its current fence.
    void releaseViews(Views& v)
    {
        if (v.srv == UINT32_MAX && v.uav == UINT32_MAX && v.rtv == UINT32_MAX && v.dsv == UINT32_MAX && v.dsvRead == UINT32_MAX) return;
        Views old = v;
        DescriptorHeaps* h = &device.descriptors();
        device.deferCall([h, old] {
            if (old.srv != UINT32_MAX) h->freeResource(old.srv);
            if (old.uav != UINT32_MAX) h->freeResource(old.uav);
            if (old.rtv != UINT32_MAX) h->freeRtv(old.rtv);
            if (old.dsv != UINT32_MAX) h->freeDsv(old.dsv);
            if (old.dsvRead != UINT32_MAX) h->freeDsv(old.dsvRead);
        });
        v = {};
    }

    void releasePlan(Plan& p)
    {
        for (Physical& ph : p.physical)
        {
            releaseViews(ph.views);
            if (ph.resource) device.deferRelease(ph.resource);
            ph.resource.Reset();
        }
    }

    uint64_t structureKey() const
    {
        uint64_t h = 1469598103934665603ull;
        for (const ResourceNode& r : resources)
        {
            h = mix(h, r.texture);
            h = mix(h, r.imported);
            if (r.texture)
            {
                h = mix(h, r.tdesc.width);
                h = mix(h, r.tdesc.height);
                h = mix(h, r.tdesc.depthOrArraySize);
                h = mix(h, r.tdesc.mipLevels);
                h = mix(h, r.tdesc.format);
                h = mix(h, r.tdesc.dimension);
            }
            else
            {
                h = mix(h, r.bdesc.size);
                h = mix(h, r.bdesc.stride);
            }
            h = mix(h, (uint64_t)r.importLayout);
        }
        for (const PassNode& p : passes)
        {
            h = mix(h, std::hash<std::string>{}(p.name));
            h = mix(h, (uint64_t)p.queue);
            h = mix(h, p.keep);
            for (const UseRecord& u : p.uses)
            {
                h = mix(h, u.resource);
                h = mix(h, (uint64_t)u.use);
            }
        }
        return h;
    }

    std::vector<Access> mergedAccesses(const PassNode& p) const
    {
        std::vector<Access> out;
        for (const UseRecord& u : p.uses)
        {
            UseInfo info = useInfo(u.use);
            auto it = std::find_if(out.begin(), out.end(), [&](const Access& a) { return a.resource == u.resource; });
            if (it == out.end())
            {
                out.push_back({ u.resource, info.write, info.sync, info.access, info.layout, info.disjoint });
                continue;
            }
            if (it->write || info.write)
            {
                // Only a UAV may be both read and written by one pass; combining any other write with another use
                // of the same resource has no single legal layout.
                bool bothUav = (it->access == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS && info.access == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                bool depthPair = (it->layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE || info.layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE) &&
                                 (it->access | info.access) == (D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
                if (!bothUav && !depthPair) fail("render graph: pass '%s' uses resource '%s' in conflicting ways", p.name.c_str(), resources[u.resource].name.c_str());
                it->write = true;
                it->disjoint = it->disjoint && info.disjoint;
                it->sync |= info.sync;
                it->access |= info.access;
                if (depthPair) it->layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
                continue;
            }
            it->sync |= info.sync;
            it->access |= info.access;
            if (it->layout != info.layout)
                it->layout = (it->layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ || info.layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ)
                                 ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
                                 : D3D12_BARRIER_LAYOUT_GENERIC_READ;
        }
        return out;
    }

    void compile(RenderGraphStats& stats)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto next = std::make_unique<Plan>();
        Plan& pl = *next;
        pl.key = structureKey();
        const uint32_t passCount = (uint32_t)passes.size();
        const uint32_t resourceCount = (uint32_t)resources.size();

        // 1. Culling: a pass lives if it is kept, writes an imported resource, or writes something a live pass reads.
        pl.live.assign(passCount, false);
        std::vector<bool> needed(resourceCount, false);
        std::vector<std::vector<Access>> accesses(passCount);
        for (uint32_t p = 0; p < passCount; ++p) accesses[p] = mergedAccesses(passes[p]);
        for (int p = (int)passCount - 1; p >= 0; --p)
        {
            bool live = passes[p].keep;
            for (const Access& a : accesses[p])
                if (a.write && (needed[a.resource] || resources[a.resource].imported)) live = true;
            if (!live) continue;
            pl.live[p] = true;
            for (const Access& a : accesses[p]) needed[a.resource] = true;
        }
        std::vector<uint32_t> order;
        for (uint32_t p = 0; p < passCount; ++p)
            if (pl.live[p]) order.push_back(p);
        std::vector<uint32_t> position(passCount, UINT32_MAX);
        for (uint32_t i = 0; i < order.size(); ++i) position[order[i]] = i;

        // 2. Resource summaries over live passes.
        struct Summary
        {
            bool used = false, srv = false, uav = false, rt = false, ds = false, dsRead = false, async = false;
            uint32_t first = UINT32_MAX, last = 0;
        };
        std::vector<Summary> sum(resourceCount);
        for (uint32_t i = 0; i < order.size(); ++i)
        {
            const PassNode& p = passes[order[i]];
            for (const UseRecord& u : p.uses)
            {
                Summary& s = sum[u.resource];
                s.used = true;
                s.first = std::min(s.first, i);
                s.last = std::max(s.last, i);
                s.async |= p.queue != QueueType::Graphics;
                switch (u.use)
                {
                case Use::SrvCompute:
                case Use::SrvGraphics: s.srv = true; break;
                case Use::UavCompute:
                case Use::UavComputeDisjoint:
                case Use::UavGraphics: s.uav = true; break;
                case Use::RenderTarget: s.rt = true; break;
                case Use::DepthWrite: s.ds = true; break;
                case Use::DepthRead: s.ds = s.dsRead = true; break;
                default: break;
                }
            }
        }

        // 3. Placement: graphics-only transients alias by lifetime; async-touched transients get their own range.
        pl.physical.resize(resourceCount);
        struct Placed
        {
            uint32_t resource;
            uint64_t offset, size;
            uint32_t first, last;
        };
        std::vector<Placed> aliased;
        std::vector<uint32_t> placeOrder;
        std::vector<D3D12_RESOURCE_DESC1> descs(resourceCount);
        std::vector<D3D12_RESOURCE_ALLOCATION_INFO> infos(resourceCount);
        uint64_t unaliasedBytes = 0;
        for (uint32_t r = 0; r < resourceCount; ++r)
        {
            const ResourceNode& n = resources[r];
            if (n.imported || !sum[r].used) continue;
            D3D12_RESOURCE_DESC1 d{};
            if (n.texture)
            {
                d.Dimension = n.tdesc.dimension;
                d.Width = n.tdesc.width;
                d.Height = n.tdesc.height;
                d.DepthOrArraySize = n.tdesc.depthOrArraySize;
                d.MipLevels = n.tdesc.mipLevels;
                d.Format = isDepthFormat(n.tdesc.format) && sum[r].srv ? typelessDepth(n.tdesc.format) : n.tdesc.format;
                d.SampleDesc.Count = 1;
                d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (sum[r].uav) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (sum[r].rt) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (sum[r].ds)
                {
                    d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                    if (!sum[r].srv) d.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
                }
            }
            else
            {
                d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                d.Width = n.bdesc.size;
                d.Height = d.DepthOrArraySize = d.MipLevels = 1;
                d.SampleDesc.Count = 1;
                d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (sum[r].uav) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            descs[r] = d;
            infos[r] = device.d3d()->GetResourceAllocationInfo2(0, 1, &d, nullptr);
            unaliasedBytes += infos[r].SizeInBytes;
            placeOrder.push_back(r);
        }
        std::stable_sort(placeOrder.begin(), placeOrder.end(), [&](uint32_t a, uint32_t b) {
            bool aa = sum[a].async, ab = sum[b].async;
            if (aa != ab) return !aa;  // aliased (graphics-only) first
            return infos[a].SizeInBytes > infos[b].SizeInBytes;
        });
        auto alignUp = [](uint64_t v, uint64_t a) { return (v + a - 1) / a * a; };
        uint64_t aliasedEnd = 0;
        for (uint32_t r : placeOrder)
        {
            if (sum[r].async) continue;
            const uint64_t size = infos[r].SizeInBytes, align = infos[r].Alignment;
            std::vector<std::pair<uint64_t, uint64_t>> busy;
            for (const Placed& p : aliased)
                if (!(p.last < sum[r].first || sum[r].last < p.first)) busy.push_back({ p.offset, p.offset + p.size });
            std::sort(busy.begin(), busy.end());
            uint64_t offset = 0;
            for (auto [b, e] : busy)
            {
                if (alignUp(offset, align) + size <= b) break;
                offset = std::max(offset, e);
            }
            offset = alignUp(offset, align);
            aliased.push_back({ r, offset, size, sum[r].first, sum[r].last });
            pl.physical[r].offset = offset;
            pl.physical[r].size = size;
            aliasedEnd = std::max(aliasedEnd, offset + size);
        }
        uint64_t end = aliasedEnd;
        for (uint32_t r : placeOrder)
        {
            if (!sum[r].async) continue;
            end = alignUp(end, infos[r].Alignment);
            pl.physical[r].offset = end;
            pl.physical[r].size = infos[r].SizeInBytes;
            end += infos[r].SizeInBytes;
        }
        // Alias predecessors: for each graphics-only transient, the union of last-use sync scopes of resources that
        // occupied overlapping memory earlier in the frame.
        std::vector<std::vector<uint32_t>> predecessors(resourceCount);
        for (const Placed& a : aliased)
            for (const Placed& b : aliased)
                if (b.last < a.first && b.offset < a.offset + a.size && a.offset < b.offset + b.size) predecessors[a.resource].push_back(b.resource);

        // 4. Heap and placed resources (reused when the plan's placement matches the previous plan).
        if (end > heapSize)
        {
            if (heap) device.deferRelease(heap);
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = alignUp(end + end / 8, 64 * 1024);
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
            check(device.d3d()->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "CreateHeap(render graph transients)");
            heap->SetName(L"unx render graph transients");
            heapSize = hd.SizeInBytes;
            if (plan)
            {
                releasePlan(*plan);  // placed resources of the old heap
                plan.reset();
            }
        }
        DescriptorHeaps& dh = device.descriptors();
        for (uint32_t r : placeOrder)
        {
            const ResourceNode& n = resources[r];
            Physical& ph = pl.physical[r];
            ph.descKey = mix(mix(std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(&descs[r]), sizeof(D3D12_RESOURCE_DESC1))), ph.offset), heapSize);
            // Reuse an identical placed resource from the previous plan.
            if (plan && r < plan->physical.size() && plan->physical[r].resource && plan->physical[r].descKey == ph.descKey)
            {
                ph.resource = std::move(plan->physical[r].resource);
                ph.views = plan->physical[r].views;
                plan->physical[r].views = {};
                continue;
            }
            D3D12_CLEAR_VALUE clear{};
            const D3D12_CLEAR_VALUE* clearPtr = nullptr;
            if (n.texture && (sum[r].rt || sum[r].ds))
            {
                clear.Format = n.tdesc.format;
                clearPtr = &clear;  // zero colour / reversed-Z far plane
            }
            check(device.d3d()->CreatePlacedResource2(heap.Get(), ph.offset, &descs[r], D3D12_BARRIER_LAYOUT_UNDEFINED, clearPtr, 0, nullptr, IID_PPV_ARGS(&ph.resource)),
                  "CreatePlacedResource2(render graph transient)");
            std::wstring wname(n.name.begin(), n.name.end());
            ph.resource->SetName(wname.c_str());
            createViews(r, ph.resource.Get(), sum[r].srv, sum[r].uav, sum[r].rt, sum[r].ds, sum[r].dsRead, ph.views);
        }
        (void)dh;
        if (plan)
        {
            releasePlan(*plan);
            plan.reset();
        }

        // 5. Barriers and queue synchronisation.
        //    Same queue: a barrier's "before" scope is exactly the accesses since the previous barrier on that resource;
        //    earlier accesses are already ordered by that barrier. Other queue: fence waits (a read waits for the last
        //    write, a write waits for every access since it); the command-list boundaries at fences order and flush
        //    everything, so only a layout change still needs a barrier. Direct-only layouts (render target, depth) are
        //    changed on the graphics queue right after its last use, before its signal.
        struct Track
        {
            bool touched = false;
            D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
            QueueType pendQueue = QueueType::Graphics;  // accesses since the last barrier (all on this queue)
            D3D12_BARRIER_SYNC pendSync = D3D12_BARRIER_SYNC_NONE;
            D3D12_BARRIER_ACCESS pendAccess = D3D12_BARRIER_ACCESS_NO_ACCESS;
            bool pendWrite = false;
            bool pendDisjoint = false;  // every access since the last barrier is a disjoint write
            uint32_t pendPos = UINT32_MAX;
            QueueType writeQueue = QueueType::Graphics;  // fence bookkeeping
            uint32_t writePos = UINT32_MAX;
            uint32_t lastPos[kQueueTypeCount] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };  // since the last write
        };
        // Position N (= order.size()) is a synthetic graphics tail that returns imported textures to their import
        // layouts once every queue is done with them.
        const uint32_t N = (uint32_t)order.size();
        std::vector<Track> track(resourceCount);
        std::vector<PlanPass> planPasses(N + 1);
        std::vector<std::vector<uint32_t>> waitsOn(N + 1);  // producer positions this pass must wait for
        std::vector<bool> splitAfter(N + 1, false);         // a direct-only layout transition sits after this pass
        std::vector<QueueType> queueOfPos(N + 1, QueueType::Graphics);
        for (uint32_t i = 0; i < N; ++i) queueOfPos[i] = passes[order[i]].queue;
        planPasses[N].pass = UINT32_MAX;
        uint32_t barrierCount = 0;

        auto pushTexture = [&](Barriers& b, uint32_t r, D3D12_BARRIER_SYNC sb, D3D12_BARRIER_SYNC sa, D3D12_BARRIER_ACCESS ab, D3D12_BARRIER_ACCESS aa,
                               D3D12_BARRIER_LAYOUT lb, D3D12_BARRIER_LAYOUT la, D3D12_TEXTURE_BARRIER_FLAGS flags) {
            if (sb == D3D12_BARRIER_SYNC_NONE) ab = D3D12_BARRIER_ACCESS_NO_ACCESS;
            if (sa == D3D12_BARRIER_SYNC_NONE) aa = D3D12_BARRIER_ACCESS_NO_ACCESS;
            D3D12_TEXTURE_BARRIER t{ sb, sa, ab, aa, lb, la, nullptr, kAllSubresources, flags };
            b.textures.push_back(t);
            b.textureResources.push_back(r);
            ++barrierCount;
        };
        auto pushBuffer = [&](Barriers& b, uint32_t r, D3D12_BARRIER_SYNC sb, D3D12_BARRIER_SYNC sa, D3D12_BARRIER_ACCESS ab, D3D12_BARRIER_ACCESS aa) {
            if (sb == D3D12_BARRIER_SYNC_NONE) ab = D3D12_BARRIER_ACCESS_NO_ACCESS;
            D3D12_BUFFER_BARRIER t{ sb, sa, ab, aa, nullptr, 0, UINT64_MAX };
            b.buffers.push_back(t);
            b.bufferResources.push_back(r);
            ++barrierCount;
        };

        for (uint32_t i = 0; i < order.size(); ++i)
        {
            const PassNode& p = passes[order[i]];
            const QueueType q = p.queue;
            const uint32_t qi = (uint32_t)q;
            PlanPass& pp = planPasses[i];
            pp.pass = order[i];
            auto waitFor = [&](uint32_t pos) {
                if (std::find(waitsOn[i].begin(), waitsOn[i].end(), pos) == waitsOn[i].end()) waitsOn[i].push_back(pos);
            };
            for (const Access& a : accesses[order[i]])
            {
                const uint32_t r = a.resource;
                const ResourceNode& n = resources[r];
                Track& t = track[r];
                bool exclusive = a.write;
                uint32_t transitionPos = UINT32_MAX;  // graphics position of a direct-only layout transition
                if (n.texture && q != QueueType::Graphics && directOnlyLayout(a.layout))
                    fail("render graph: pass '%s' on the %s queue needs a direct-queue layout for '%s'", p.name.c_str(), queueName(q), n.name.c_str());

                if (!t.touched)
                {
                    t.touched = true;
                    t.pendDisjoint = true;  // first use: ordered after earlier frames and alias predecessors
                    if (n.imported)
                    {
                        t.layout = n.texture ? n.importLayout : D3D12_BARRIER_LAYOUT_UNDEFINED;
                        if (n.texture && q != QueueType::Graphics && directOnlyLayout(t.layout))
                            fail("render graph: imported '%s' is first used on the %s queue but imported in a direct-queue layout", n.name.c_str(), queueName(q));
                        if (n.texture && t.layout != a.layout)
                            pushTexture(pp.before, r, D3D12_BARRIER_SYNC_NONE, a.sync, D3D12_BARRIER_ACCESS_NO_ACCESS, a.access, t.layout, a.layout, D3D12_TEXTURE_BARRIER_FLAG_NONE);
                    }
                    else
                    {
                        // Aliased memory: wait for the last accesses of every earlier occupant (all graphics-queue).
                        D3D12_BARRIER_SYNC aliasSync = D3D12_BARRIER_SYNC_NONE;
                        for (uint32_t pr : predecessors[r]) aliasSync |= track[pr].pendSync;
                        if (n.texture)
                            pushTexture(pp.before, r, aliasSync, a.sync, D3D12_BARRIER_ACCESS_NO_ACCESS, a.access, D3D12_BARRIER_LAYOUT_UNDEFINED, a.layout, D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
                        else if (aliasSync != D3D12_BARRIER_SYNC_NONE)
                            pushBuffer(pp.before, r, aliasSync, a.sync, D3D12_BARRIER_ACCESS_NO_ACCESS, a.access);
                    }
                    if (n.texture) t.layout = a.layout;
                }
                else
                {
                    // A layout change rewrites the texture like a write does, so it also waits for other queues' readers.
                    const bool layoutChange = n.texture && t.layout != a.layout;
                    if (a.write || layoutChange)
                    {
                        for (uint32_t k = 0; k < kQueueTypeCount; ++k)
                            if (k != qi && t.lastPos[k] != UINT32_MAX) waitFor(t.lastPos[k]);  // RAW/WAW/WAR across queues
                    }
                    else if (t.writePos != UINT32_MAX && t.writeQueue != q)
                        waitFor(t.writePos);  // RAW across queues
                    exclusive = a.write || layoutChange;
                    const bool disjointRun = a.disjoint && t.pendDisjoint && !layoutChange;
                    if (t.pendQueue == q)
                    {
                        if ((t.pendWrite || a.write || layoutChange) && !disjointRun)
                        {
                            if (n.texture)
                                pushTexture(pp.before, r, t.pendSync, a.sync, t.pendAccess, a.access, t.layout, a.layout, D3D12_TEXTURE_BARRIER_FLAG_NONE);
                            else
                                pushBuffer(pp.before, r, t.pendSync, a.sync, t.pendAccess, a.access);
                            if (n.texture) t.layout = a.layout;
                            t.pendSync = D3D12_BARRIER_SYNC_NONE;
                            t.pendAccess = D3D12_BARRIER_ACCESS_NO_ACCESS;
                            t.pendWrite = false;
                            t.pendDisjoint = true;  // a run of disjoint writers may start after this barrier
                        }
                    }
                    else
                    {
                        if (layoutChange && directOnlyLayout(t.layout))
                        {
                            // Only the graphics queue can leave a direct-only layout: transition after its last use.
                            pushTexture(planPasses[t.pendPos].after, r, t.pendSync, D3D12_BARRIER_SYNC_NONE, t.pendAccess, D3D12_BARRIER_ACCESS_NO_ACCESS, t.layout, a.layout,
                                        D3D12_TEXTURE_BARRIER_FLAG_NONE);
                            splitAfter[t.pendPos] = true;  // the transition completes at that command list's end
                            waitFor(t.pendPos);
                            transitionPos = t.pendPos;
                        }
                        else if (layoutChange)
                            pushTexture(pp.before, r, D3D12_BARRIER_SYNC_NONE, a.sync, D3D12_BARRIER_ACCESS_NO_ACCESS, a.access, t.layout, a.layout, D3D12_TEXTURE_BARRIER_FLAG_NONE);
                        if (n.texture) t.layout = a.layout;
                        t.pendSync = D3D12_BARRIER_SYNC_NONE;
                        t.pendAccess = D3D12_BARRIER_ACCESS_NO_ACCESS;
                        t.pendWrite = false;
                        t.pendDisjoint = true;
                    }
                }
                t.pendQueue = q;
                t.pendDisjoint = t.pendDisjoint && a.disjoint;
                t.pendSync |= a.sync;
                t.pendAccess = orAccess(t.pendAccess, a.access);
                t.pendWrite = t.pendWrite || a.write;
                t.pendPos = i;
                if (exclusive)
                {
                    // The layout transition is the "write" other queues must wait for: on the graphics queue for a
                    // direct-only layout (unless this pass also writes), here otherwise.
                    const bool onGraphics = transitionPos != UINT32_MAX && !a.write;
                    t.writeQueue = onGraphics ? QueueType::Graphics : q;
                    t.writePos = onGraphics ? transitionPos : i;
                    for (uint32_t k = 0; k < kQueueTypeCount; ++k) t.lastPos[k] = UINT32_MAX;
                }
                t.lastPos[qi] = i;
            }
        }
        // Imported textures return to their import layout in the graphics tail, after every queue's last access.
        for (uint32_t r = 0; r < resourceCount; ++r)
        {
            const ResourceNode& n = resources[r];
            const Track& t = track[r];
            if (!n.imported || !n.texture || !t.touched || t.layout == n.importLayout) continue;
            const uint32_t i = N;
            auto waitFor = [&](uint32_t pos) {
                if (std::find(waitsOn[i].begin(), waitsOn[i].end(), pos) == waitsOn[i].end()) waitsOn[i].push_back(pos);
            };
            for (uint32_t k = 0; k < kQueueTypeCount; ++k)
                if (k != (uint32_t)QueueType::Graphics && t.lastPos[k] != UINT32_MAX) waitFor(t.lastPos[k]);
            if (t.pendQueue == QueueType::Graphics)
                pushTexture(planPasses[N].before, r, t.pendSync, D3D12_BARRIER_SYNC_NONE, t.pendAccess, D3D12_BARRIER_ACCESS_NO_ACCESS, t.layout, n.importLayout,
                            D3D12_TEXTURE_BARRIER_FLAG_NONE);
            else
                pushTexture(planPasses[N].before, r, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_NO_ACCESS, t.layout,
                            n.importLayout, D3D12_TEXTURE_BARRIER_FLAG_NONE);
        }
        const bool hasTail = !planPasses[N].before.empty();

        // 6. Segments (one command list each). A consumer waits only for the latest producer position per queue
        //    (queues execute in order); a producer's command list ends right after a position someone waits for. A
        //    segment splits before a pass that needs a later fence than its segment already waited for.
        const uint32_t positions = hasTail ? N + 1 : N;
        std::vector<std::array<uint32_t, kQueueTypeCount>> required(positions);
        std::vector<bool> signalAfter(positions, false);
        for (uint32_t i = 0; i < positions; ++i)
        {
            required[i].fill(UINT32_MAX);
            for (uint32_t pos : waitsOn[i])
            {
                uint32_t k = (uint32_t)queueOfPos[pos];
                if (required[i][k] == UINT32_MAX || pos > required[i][k]) required[i][k] = pos;
            }
            for (uint32_t k = 0; k < kQueueTypeCount; ++k)
                if (required[i][k] != UINT32_MAX) signalAfter[required[i][k]] = true;
        }
        for (uint32_t i = 0; i < positions; ++i)
            if (splitAfter[i]) signalAfter[i] = true;
        std::vector<int> segmentOfPos(positions, -1);
        int currentSegment[kQueueTypeCount] = { -1, -1, -1 };
        bool splitNext[kQueueTypeCount] = { true, true, true };
        std::array<uint32_t, kQueueTypeCount> waited[kQueueTypeCount];
        for (auto& w : waited) w.fill(UINT32_MAX);
        uint32_t syncCount = 0;
        for (uint32_t i = 0; i < positions; ++i)
        {
            const QueueType q = queueOfPos[i];
            const uint32_t qi = (uint32_t)q;
            bool needsLaterFence = false;
            for (uint32_t k = 0; k < kQueueTypeCount; ++k)
                if (required[i][k] != UINT32_MAX && (waited[qi][k] == UINT32_MAX || required[i][k] > waited[qi][k])) needsLaterFence = true;
            if (splitNext[qi] || needsLaterFence || currentSegment[qi] < 0)
            {
                Segment seg;
                seg.queue = q;
                pl.segments.push_back(std::move(seg));
                currentSegment[qi] = (int)pl.segments.size() - 1;
                splitNext[qi] = false;
            }
            Segment& seg = pl.segments[currentSegment[qi]];
            for (uint32_t k = 0; k < kQueueTypeCount; ++k)
            {
                const uint32_t req = required[i][k];
                if (req == UINT32_MAX || (waited[qi][k] != UINT32_MAX && req <= waited[qi][k])) continue;
                waited[qi][k] = req;
                seg.waitSegments.push_back((uint32_t)segmentOfPos[req]);
                ++syncCount;
            }
            seg.passes.push_back(std::move(planPasses[i]));
            segmentOfPos[i] = currentSegment[qi];
            if (signalAfter[i]) splitNext[qi] = true;
        }
        for (uint32_t q = 0; q < kQueueTypeCount; ++q)
        {
            int first = -1, last = -1;
            for (int si = 0; si < (int)pl.segments.size(); ++si)
                if ((uint32_t)pl.segments[si].queue == q)
                {
                    if (first < 0) first = si;
                    last = si;
                }
            if (first >= 0) pl.segments[first].firstOfQueue = true;
            if (last >= 0) pl.segments[last].lastOfQueue = true;
        }

        uint32_t batches = 0;
        for (const Segment& sg : pl.segments)
            for (const PlanPass& pp : sg.passes) batches += (pp.before.empty() ? 0 : 1) + (pp.after.empty() ? 0 : 1);

        pl.stats.declaredPasses = passCount;
        pl.stats.livePasses = (uint32_t)order.size();
        pl.stats.transientResources = (uint32_t)placeOrder.size();
        pl.stats.barriers = barrierCount;
        pl.stats.barrierBatches = batches;
        pl.stats.crossQueueSyncs = syncCount;
        pl.stats.commandLists = (uint32_t)pl.segments.size();
        pl.stats.transientBytesAliased = end;
        pl.stats.transientBytesUnaliased = unaliasedBytes;
        plan = std::move(next);
        stats = plan->stats;
        stats.cpuCompileMs = msSince(t0);
    }

    void createViews(uint32_t r, ID3D12Resource* res, bool srv, bool uav, bool rt, bool ds, bool dsRead, Views& v)
    {
        const ResourceNode& n = resources[r];
        DescriptorHeaps& h = device.descriptors();
        ID3D12Device* d = device.d3d();
        if (n.texture)
        {
            const bool depth = isDepthFormat(n.tdesc.format);
            const bool array = n.tdesc.depthOrArraySize > 1 && n.tdesc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            const bool volume = n.tdesc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            if (srv && v.srv == UINT32_MAX)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = depth ? depthSrvFormat(n.tdesc.format) : n.tdesc.format;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (volume) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = n.tdesc.mipLevels; }
                else if (array) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = n.tdesc.mipLevels; sd.Texture2DArray.ArraySize = n.tdesc.depthOrArraySize; }
                else { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = n.tdesc.mipLevels; }
                v.srv = h.allocateResource();
                d->CreateShaderResourceView(res, &sd, h.resourceCpu(v.srv));
            }
            if (uav && v.uav == UINT32_MAX)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = n.tdesc.format;
                if (volume) { ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; ud.Texture3D.WSize = n.tdesc.depthOrArraySize; }
                else if (array) { ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; ud.Texture2DArray.ArraySize = n.tdesc.depthOrArraySize; }
                else ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                v.uav = h.allocateResource();
                d->CreateUnorderedAccessView(res, nullptr, &ud, h.resourceCpu(v.uav));
            }
            if (rt && v.rtv == UINT32_MAX)
            {
                v.rtv = h.allocateRtv();
                d->CreateRenderTargetView(res, nullptr, h.rtv(v.rtv));
            }
            if (ds && v.dsv == UINT32_MAX)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format = n.tdesc.format;
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                v.dsv = h.allocateDsv();
                d->CreateDepthStencilView(res, &dd, h.dsv(v.dsv));
            }
            if (dsRead && v.dsvRead == UINT32_MAX)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format = n.tdesc.format;
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
                v.dsvRead = h.allocateDsv();
                d->CreateDepthStencilView(res, &dd, h.dsv(v.dsvRead));
            }
        }
        else
        {
            const uint32_t stride = n.bdesc.stride;
            const uint64_t elements = stride ? n.bdesc.size / stride : n.bdesc.size / 4;
            if (srv && v.srv == UINT32_MAX)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                sd.Buffer.NumElements = (UINT)elements;
                sd.Buffer.StructureByteStride = stride;
                sd.Buffer.Flags = stride ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
                v.srv = h.allocateResource();
                d->CreateShaderResourceView(res, &sd, h.resourceCpu(v.srv));
            }
            if (uav && v.uav == UINT32_MAX)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                ud.Buffer.NumElements = (UINT)elements;
                ud.Buffer.StructureByteStride = stride;
                ud.Buffer.Flags = stride ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
                v.uav = h.allocateResource();
                d->CreateUnorderedAccessView(res, nullptr, &ud, h.resourceCpu(v.uav));
            }
        }
    }

    const Views& viewsOf(uint32_t r) const
    {
        const ResourceNode& n = resources[r];
        if (n.imported) return importedViews.at(n.importedResource);
        return plan->physical[r].views;
    }

    void emit(ID3D12GraphicsCommandList7* cmd, Barriers& b)
    {
        if (b.empty()) return;
        for (size_t k = 0; k < b.textures.size(); ++k) b.textures[k].pResource = framePointers[b.textureResources[k]];
        for (size_t k = 0; k < b.buffers.size(); ++k) b.buffers[k].pResource = framePointers[b.bufferResources[k]];
        D3D12_BARRIER_GROUP groups[2];
        uint32_t count = 0;
        if (!b.textures.empty())
        {
            groups[count].Type = D3D12_BARRIER_TYPE_TEXTURE;
            groups[count].NumBarriers = (UINT32)b.textures.size();
            groups[count].pTextureBarriers = b.textures.data();
            ++count;
        }
        if (!b.buffers.empty())
        {
            groups[count].Type = D3D12_BARRIER_TYPE_BUFFER;
            groups[count].NumBarriers = (UINT32)b.buffers.size();
            groups[count].pBufferBarriers = b.buffers.data();
            ++count;
        }
        cmd->Barrier(count, groups);
    }
};

// ------------------------------------------------------------------------------------------------ PassBuilder

TextureRef PassBuilder::createTexture(const TextureDesc& desc) { return m_graph.createTexture(desc); }
BufferRef PassBuilder::createBuffer(const BufferDesc& desc) { return m_graph.createBuffer(desc); }

void PassBuilder::use(TextureRef texture, Use use)
{
    auto& impl = *m_graph.m_impl;
    if (!texture.valid() || texture.id >= impl.resources.size() || !impl.resources[texture.id].texture) fail("render graph: invalid texture in pass '%s'", impl.passes[m_pass].name.c_str());
    impl.passes[m_pass].uses.push_back({ texture.id, use });
}

void PassBuilder::use(BufferRef buffer, Use use)
{
    auto& impl = *m_graph.m_impl;
    if (!buffer.valid() || buffer.id >= impl.resources.size() || impl.resources[buffer.id].texture) fail("render graph: invalid buffer in pass '%s'", impl.passes[m_pass].name.c_str());
    if (use == Use::RenderTarget || use == Use::DepthWrite || use == Use::DepthRead) fail("render graph: buffer used as a render target or depth");
    impl.passes[m_pass].uses.push_back({ buffer.id, use });
}

void PassBuilder::keep() { m_graph.m_impl->passes[m_pass].keep = true; }

// ------------------------------------------------------------------------------------------------ PassContext

uint32_t PassContext::srv(TextureRef t) const { return m_graph->m_impl->viewsOf(t.id).srv; }
uint32_t PassContext::uav(TextureRef t) const { return m_graph->m_impl->viewsOf(t.id).uav; }
uint32_t PassContext::srv(BufferRef b) const { return m_graph->m_impl->viewsOf(b.id).srv; }
uint32_t PassContext::uav(BufferRef b) const { return m_graph->m_impl->viewsOf(b.id).uav; }
D3D12_CPU_DESCRIPTOR_HANDLE PassContext::rtv(TextureRef t) const { return m_graph->m_device.descriptors().rtv(m_graph->m_impl->viewsOf(t.id).rtv); }
D3D12_CPU_DESCRIPTOR_HANDLE PassContext::dsv(TextureRef t) const { return m_graph->m_device.descriptors().dsv(m_graph->m_impl->viewsOf(t.id).dsv); }
D3D12_CPU_DESCRIPTOR_HANDLE PassContext::dsvReadOnly(TextureRef t) const { return m_graph->m_device.descriptors().dsv(m_graph->m_impl->viewsOf(t.id).dsvRead); }
ID3D12Resource* PassContext::resource(TextureRef t) const { return m_graph->m_impl->framePointers[t.id]; }
ID3D12Resource* PassContext::resource(BufferRef b) const { return m_graph->m_impl->framePointers[b.id]; }
D3D12_GPU_VIRTUAL_ADDRESS PassContext::address(BufferRef b) const { return m_graph->m_impl->framePointers[b.id]->GetGPUVirtualAddress(); }
const TextureDesc& PassContext::desc(TextureRef t) const { return m_graph->m_impl->resources[t.id].tdesc; }
void PassContext::computeConstants(const void* data, uint32_t dwords) const { cmd->SetComputeRoot32BitConstants(0, dwords, data, 0); }
void PassContext::graphicsConstants(const void* data, uint32_t dwords) const { cmd->SetGraphicsRoot32BitConstants(0, dwords, data, 0); }

// ------------------------------------------------------------------------------------------------ RenderGraph

RenderGraph::RenderGraph(Device& device) : m_impl(std::make_unique<Impl>(device)), m_device(device) {}

RenderGraph::~RenderGraph()
{
    m_device.waitIdle();
    m_impl.reset();
    m_device.collectGarbage();
}

TextureRef RenderGraph::importTexture(ID3D12Resource* resource, const TextureDesc& desc, D3D12_BARRIER_LAYOUT layout)
{
    Impl::ResourceNode n;
    n.texture = true;
    n.imported = true;
    n.tdesc = desc;
    n.name = desc.name;
    n.importedResource = resource;
    n.importLayout = layout;
    m_impl->resources.push_back(std::move(n));
    return { (uint32_t)m_impl->resources.size() - 1 };
}

TextureRef RenderGraph::createTexture(const TextureDesc& desc)
{
    Impl::ResourceNode n;
    n.texture = true;
    n.tdesc = desc;
    n.name = desc.name;
    m_impl->resources.push_back(std::move(n));
    return { (uint32_t)m_impl->resources.size() - 1 };
}

BufferRef RenderGraph::createBuffer(const BufferDesc& desc)
{
    if (desc.size == 0) fail("render graph: buffer '%s' has zero size", desc.name);
    Impl::ResourceNode n;
    n.texture = false;
    n.bdesc = desc;
    n.name = desc.name;
    m_impl->resources.push_back(std::move(n));
    return { (uint32_t)m_impl->resources.size() - 1 };
}

BufferRef RenderGraph::importBuffer(ID3D12Resource* resource, const BufferDesc& desc)
{
    Impl::ResourceNode n;
    n.texture = false;
    n.imported = true;
    n.bdesc = desc;
    n.name = desc.name;
    n.importedResource = resource;
    m_impl->resources.push_back(std::move(n));
    return { (uint32_t)m_impl->resources.size() - 1 };
}

void RenderGraph::addPass(std::string_view name, QueueType queue, const SetupFn& setup, ExecuteFn execute)
{
    if (queue == QueueType::Copy) fail("render graph: copy-queue passes are not supported yet");
    Impl::PassNode p;
    p.name = std::string(name);
    p.queue = m_asyncCompute ? queue : QueueType::Graphics;
    p.execute = std::move(execute);
    m_impl->passes.push_back(std::move(p));
    PassBuilder b(*this, (uint32_t)m_impl->passes.size() - 1);
    setup(b);
}

void RenderGraph::execute(GpuProfiler* profiler)
{
    Impl& impl = *m_impl;
    const uint64_t key = impl.structureKey();
    const bool reuse = impl.plan && impl.plan->key == key;
    if (!reuse) impl.compile(m_stats);
    else
    {
        m_stats = impl.plan->stats;
        m_stats.cpuCompileMs = 0;
    }
    m_stats.planReused = reuse;
    Impl::Plan& plan = *impl.plan;

    // This frame's resource pointers and imported views.
    impl.framePointers.assign(impl.resources.size(), nullptr);
    for (uint32_t r = 0; r < impl.resources.size(); ++r)
    {
        const auto& n = impl.resources[r];
        if (n.imported)
        {
            impl.framePointers[r] = n.importedResource;
            impl.importedViews.try_emplace(n.importedResource);
        }
        else if (plan.physical[r].resource)
            impl.framePointers[r] = plan.physical[r].resource.Get();
    }
    // Views for imported resources: created on first sight with the uses seen this frame.
    for (uint32_t p = 0; p < impl.passes.size(); ++p)
    {
        if (!plan.live[p]) continue;
        for (const auto& u : impl.passes[p].uses)
        {
            const auto& n = impl.resources[u.resource];
            if (!n.imported) continue;
            Impl::Views& v = impl.importedViews[n.importedResource];
            bool srv = u.use == Use::SrvCompute || u.use == Use::SrvGraphics;
            bool uav = u.use == Use::UavCompute || u.use == Use::UavComputeDisjoint || u.use == Use::UavGraphics;
            impl.createViews(u.resource, n.importedResource, srv, uav, u.use == Use::RenderTarget, u.use == Use::DepthWrite, u.use == Use::DepthRead, v);
        }
    }

    // Record every segment.
    auto t0 = std::chrono::steady_clock::now();
    std::vector<CommandList> lists(plan.segments.size());
    PassContext ctx;
    ctx.m_graph = this;
    for (size_t s = 0; s < plan.segments.size(); ++s)
    {
        Impl::Segment& seg = plan.segments[s];
        lists[s] = m_device.acquireCommandList(seg.queue);
        ID3D12GraphicsCommandList7* cmd = lists[s].list.Get();
        if (profiler && seg.firstOfQueue) profiler->frameMark(cmd, seg.queue);
        for (Impl::PlanPass& pp : seg.passes)
        {
            impl.emit(cmd, pp.before);
            if (pp.pass != UINT32_MAX)
            {
                Impl::PassNode& pass = impl.passes[pp.pass];
                if (profiler) profiler->passBegin(cmd, seg.queue, pass.name);
                ctx.cmd = cmd;
                ctx.queue = seg.queue;
                pass.execute(ctx);
                if (profiler) profiler->passEnd(cmd, seg.queue);
            }
            impl.emit(cmd, pp.after);
        }
        if (profiler && seg.lastOfQueue)
        {
            profiler->frameMark(cmd, seg.queue);
            profiler->resolve(cmd, seg.queue);
        }
    }
    m_stats.cpuRecordMs = msSince(t0);

    // Submit in plan order: the first segment of each queue waits for the other queues' previous frame (transient
    // memory and views are reused across frames); later segments wait for their producers.
    t0 = std::chrono::steady_clock::now();
    std::vector<uint64_t> segmentFence(plan.segments.size(), 0);
    for (size_t s = 0; s < plan.segments.size(); ++s)
    {
        Impl::Segment& seg = plan.segments[s];
        Queue& q = m_device.queue(seg.queue);
        if (seg.firstOfQueue)
            for (uint32_t o = 0; o < kQueueTypeCount; ++o)
                if (o != (uint32_t)seg.queue && impl.prevFrameFence[o]) q.waitGpu(m_device.queue((QueueType)o), impl.prevFrameFence[o]);
        for (uint32_t w : seg.waitSegments) q.waitGpu(m_device.queue(plan.segments[w].queue), segmentFence[w]);
        segmentFence[s] = m_device.submit(lists[s]);
        m_lastFence[(size_t)seg.queue] = segmentFence[s];
    }
    for (uint32_t q = 0; q < kQueueTypeCount; ++q)
        for (size_t s = 0; s < plan.segments.size(); ++s)
            if ((uint32_t)plan.segments[s].queue == q) impl.prevFrameFence[q] = segmentFence[s];
    m_stats.cpuSubmitMs = msSince(t0);

    impl.passes.clear();
    impl.resources.clear();
    m_device.collectGarbage();
}
} // namespace unx::render
