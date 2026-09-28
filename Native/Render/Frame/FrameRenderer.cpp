#include "unx/render/FrameRenderer.h"

#include "unx/render/Tracks.h"

#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace unx::render
{
ViewDesc ViewDesc::fromCamera(const scene::Camera& camera, uint32_t width, uint32_t height, const float4x4& prevViewProj)
{
    ViewDesc v;
    v.kind = gpu::ViewKind::Main;
    v.width = width;
    v.height = height;
    v.position = camera.position;
    v.nearPlane = camera.nearPlane;
    v.verticalFov = camera.verticalFov;
    v.ev100 = camera.ev100;
    v.view = lookTo(camera.position, normalize(camera.forward), normalize(camera.up));
    v.proj = perspectiveReversedInfinite(camera.verticalFov, (float)width / (float)height, camera.nearPlane);
    v.viewProj = mul(v.proj, v.view);
    v.invViewProj = inverse(v.viewProj);
    v.prevViewProj = prevViewProj;
    return v;
}

uint32_t passBandCount(const QualityConfig& quality, uint32_t width, uint32_t height)
{
    const double perBand = (double)quality.integer("output.band_pixels");
    if (perBand < 0) fail("output.band_pixels must be 0 (one band) or positive");
    if (perBand == 0) return 1;  // one band: the whole view (the default, v1.31)
    return std::max(1u, (uint32_t)std::lround((double)width * height / perBand));
}

ViewDesc ViewDesc::planarReflection(const ViewDesc& main, float4 plane, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
    // Normalise the plane and orient it so the main camera is on its positive side.
    float3 n{ plane.x, plane.y, plane.z };
    const float len = length(n);
    n = n / len;
    float w = plane.w / len;
    if (dot(n, main.position) + w < 0)
    {
        n = -n;
        w = -w;
    }
    float4x4 reflect;  // p' = p - 2 (n.p + w) n
    reflect.m[0][0] = 1 - 2 * n.x * n.x; reflect.m[0][1] = -2 * n.x * n.y;    reflect.m[0][2] = -2 * n.x * n.z;    reflect.m[0][3] = -2 * w * n.x;
    reflect.m[1][0] = -2 * n.y * n.x;    reflect.m[1][1] = 1 - 2 * n.y * n.y; reflect.m[1][2] = -2 * n.y * n.z;    reflect.m[1][3] = -2 * w * n.y;
    reflect.m[2][0] = -2 * n.z * n.x;    reflect.m[2][1] = -2 * n.z * n.y;    reflect.m[2][2] = 1 - 2 * n.z * n.z; reflect.m[2][3] = -2 * w * n.z;

    // Crop the main projection to the pixel rectangle (off-centre frustum).
    const float W = (float)main.width, H = (float)main.height;
    const float xl = rx / W * 2 - 1, xr = (rx + rw) / W * 2 - 1;
    const float yt = 1 - ry / H * 2, yb = 1 - (ry + rh) / H * 2;
    const float sx = 2 / (xr - xl), cx = (xl + xr) * 0.5f, sy = 2 / (yt - yb), cy = (yt + yb) * 0.5f;
    float4x4 crop;
    crop.m[0][0] = sx; crop.m[0][3] = -sx * cx;
    crop.m[1][1] = sy; crop.m[1][3] = -sy * cy;

    ViewDesc v = main;
    v.kind = gpu::ViewKind::PlanarReflection;
    v.width = rw;
    v.height = rh;
    // The same pixel angle as the main view: consumers take 2 tan(verticalFov / 2) / height as the pixel's angular size
    // (froxel tiles, texture LOD, VSM footprints, ray cones), and the crop keeps the main view's pixels.
    v.verticalFov = 2 * std::atan(std::tan(main.verticalFov * 0.5f) * (float)rh / H);
    v.view = mul(main.view, reflect);
    v.proj = mul(crop, main.proj);
    v.viewProj = mul(v.proj, v.view);
    v.invViewProj = inverse(v.viewProj);
    v.prevViewProj = v.viewProj;
    v.position = main.position - n * (2 * (dot(n, main.position) + w));
    v.clipPlane = { n.x, n.y, n.z, w };
    v.mirrored = !main.mirrored;
    return v;
}

FrameRenderer::FrameRenderer(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, GpuScene& scene, uint32_t framesInFlight)
    : m_device(device), m_shaders(shaders), m_quality(quality), m_scene(scene), m_framesInFlight(framesInFlight)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (UINT64)framesInFlight * kMaxViewsPerFrame * 1024;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_constants)),
          "frame constants ring");
    D3D12_RANGE none{ 0, 0 };
    check(m_constants->Map(0, &none, reinterpret_cast<void**>(&m_mapped)), "map frame constants");
}

FrameRenderer::~FrameRenderer()
{
    m_device.waitIdle();  // track state may hold resources of frames in flight
    m_trackState.clear();
    if (m_constants) m_constants->Unmap(0, nullptr);
    m_device.deferRelease(m_constants);
}

D3D12_GPU_VIRTUAL_ADDRESS FrameRenderer::allocateFrameConstants(const FrameContext& frame, const ViewDesc& view)
{
    if (m_slotFrame != frame.frameIndex)
    {
        m_slotFrame = frame.frameIndex;
        m_slotViews = 0;
    }
    if (m_slotViews >= kMaxViewsPerFrame) fail("FrameRenderer: more than %u views in one frame", kMaxViewsPerFrame);
    const uint64_t offset = ((frame.frameIndex % m_framesInFlight) * kMaxViewsPerFrame + m_slotViews++) * 1024;
    gpu::FrameConstants c = frameConstants(m_scene, frame, view);
    c.debugDraw = m_debugDraw;
    c.viewModelScale = view.kind == gpu::ViewKind::Main ? m_viewModelScale : 1.0f;  // other views see the true geometry
    // (the main view renders below the output: its texture footprints over the output pixel, GpuSceneLayout.h)
    const bool upscaled = frame.upscale.outputHeight > view.height && view.kind == gpu::ViewKind::Main && view.width == frame.mainView.width &&
                          view.height == frame.mainView.height;
    c.upscaleRatio = upscaled ? (float)view.height / (float)frame.upscale.outputHeight : 0.0f;
    std::memcpy(m_mapped + offset, &c, sizeof c);
    return m_constants->GetGPUVirtualAddress() + offset;
}

gpu::FrameConstants FrameRenderer::frameConstants(const GpuScene& scene, const FrameContext& frame, const ViewDesc& view)
{
    gpu::FrameConstants c{};
    c.viewProj = view.viewProj;
    c.prevViewProj = view.prevViewProj;
    c.invViewProj = view.invViewProj;
    c.view = view.view;
    c.proj = view.proj;
    c.cameraPosition = view.position;
    c.nearPlane = view.nearPlane;
    c.clipPlane = view.clipPlane;
    c.viewWidth = view.width;
    c.viewHeight = view.height;
    c.viewKind = (uint32_t)view.kind;
    c.frameIndex = (uint32_t)frame.frameIndex;
    c.time = (float)frame.time;
    c.deltaTime = frame.deltaTime;
    c.exposure = 1.0f / (1.2f * std::exp2(view.ev100));
    c.tanHalfFovY = std::tan(view.verticalFov * 0.5f);
    if (const scene::Scene* s = scene.source())
    {
        c.sunDirection = s->sun.direction;
        c.sunIlluminance = s->sun.illuminance;
        c.sunColor = s->sun.color;
        c.sunAngularRadius = s->sun.angularRadius;
        c.windDirection = s->windDirection;
        c.windSpeed = s->windSpeed;
    }
    scene.fill(c);
    return c;
}

// A14: the frame's auxiliary views in drawing order. A view is drawn after the views it reads (Kahn's order, ties by
// id); the views left on cycles are drawn by id and read each other's previous-frame outputs (FEATURES_GAME 8).
// A3 FX light tail (v1.81): one import per frame of the scene light buffer and the FX count word, so every writer and
// reader declares the same graph resource.
static void importFxLights(RenderGraph& graph, const GpuScene& scene, FrameResources& resources)
{
    const GpuScene::FxLightRange r = scene.fxLightRange();
    if (r.capacity == 0) return;
    resources.fxLights = graph.importBuffer(r.lightBuffer, BufferDesc{ "scene lights (FX tail)", (uint64_t)(r.first + r.capacity) * sizeof(gpu::Light), (uint32_t)sizeof(gpu::Light) });
    resources.fxLightCount = graph.importBuffer(r.countBuffer, BufferDesc{ "FX light count", 16, 4 });
}

static std::vector<ViewResources> auxiliaryViews(FramePassContext& fc, const FrameContext& frame)
{
    const std::vector<AuxView>& in = frame.auxViews;
    if (in.size() + 1 > FrameRenderer::kMaxViewsPerFrame) fail("A14: %zu auxiliary views (at most %u with the main view)", in.size(), FrameRenderer::kMaxViewsPerFrame - 1);
    std::map<uint32_t, size_t> index;
    for (size_t i = 0; i < in.size(); ++i)
    {
        const AuxView& a = in[i];
        if (a.id == 0) fail("A14: auxiliary view id 0 (the main view's)");
        if (!index.emplace(a.id, i).second) fail("A14: auxiliary view id %u twice", a.id);
        const gpu::ViewKind k = a.view.kind;
        if (k != gpu::ViewKind::RenderTexture && k != gpu::ViewKind::Mirror && k != gpu::ViewKind::Portal && k != gpu::ViewKind::Split)
            fail("A14: auxiliary view %u has kind %u (RenderTexture, Mirror, Portal or Split)", a.id, (uint32_t)k);
        if (!a.output.valid()) fail("A14: auxiliary view %u has no output", a.id);
    }
    std::vector<uint32_t> pending(in.size(), 0);
    for (size_t i = 0; i < in.size(); ++i)
        for (uint32_t r : in[i].reads)
            if (r != 0 && index.count(r) && r != in[i].id) ++pending[i];
    std::vector<size_t> order;
    std::vector<uint8_t> done(in.size(), 0);
    for (;;)
    {
        size_t pick = in.size();
        for (size_t i = 0; i < in.size(); ++i)
            if (!done[i] && pending[i] == 0 && (pick == in.size() || in[i].id < in[pick].id)) pick = i;
        if (pick == in.size())
        {
            for (size_t i = 0; i < in.size(); ++i)  // cycles: the lowest id next; its unresolved reads are previous-frame reads
                if (!done[i] && (pick == in.size() || in[i].id < in[pick].id)) pick = i;
            if (pick == in.size()) break;
        }
        done[pick] = 1;
        order.push_back(pick);
        for (size_t i = 0; i < in.size(); ++i)
            if (!done[i])
                for (uint32_t r : in[i].reads)
                    if (r == in[pick].id && pending[i] > 0) --pending[i];
    }
    std::vector<ViewResources> out;
    for (size_t i : order)
    {
        ViewResources v;
        v.view = in[i].view;
        v.viewId = in[i].id;
        v.frameConstants = fc.frameConstantsFor(v.view);
        v.color = in[i].output;
        out.push_back(v);
    }
    return out;
}

// Temporal upscale (output.render_scale, output.render_height_max): the main view renders at the output height x
// render_scale, at most render_height_max (same aspect), with a sub-pixel jitter (Halton 2, 3; a cycle of ceil(8 x output / internal pixels), at most 64, as temporal upsamplers
// size theirs), and M's temporal upscale reconstructs the output (Upscale.cpp). Every system of the frame sees the
// internal view; the previous view-projection is the previous frame's jittered one, so the histories that reproject
// onto the previous frame's samples (R's reflections and probes, M's) land on them.
static float halton(uint32_t index, uint32_t base)
{
    float f = 1, r = 0;
    for (uint32_t i = index; i > 0; i /= base)
    {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

void FrameRenderer::setupUpscale(FrameContext& frame)
{
    frame.upscale = FrameContext::Upscale{};
    const int64_t maxHeight = m_quality.has("output.render_height_max") ? m_quality.integer("output.render_height_max") : 0;
    const double scale = m_quality.has("output.render_scale") ? m_quality.number("output.render_scale") : 1.0;
    const int64_t minHeight = m_quality.has("output.render_scale_min_height") ? m_quality.integer("output.render_scale_min_height") : 0;
    if (!(scale > 0 && scale <= 1)) fail("output.render_scale must be in (0, 1]");
    ViewDesc& v = frame.mainView;
    // Internal height: the output's x render_scale (outputs of at least render_scale_min_height), at most render_height_max
    // (0: no cap); native when that is not below the output. (captures (outputLinearHdr: references and gates compare the
    // native image) and debug buffer views (they show the view's own buffers pixel for pixel) stay native)
    uint32_t h = v.height == 0 || (int64_t)v.height < minHeight ? v.height : std::max(8u, (uint32_t)std::lround((double)v.height * scale));
    if (maxHeight > 0) h = std::min(h, (uint32_t)maxHeight);
    if (maxHeight < 0 || v.kind != gpu::ViewKind::Main || h >= v.height || v.width == 0 || frame.outputLinearHdr ||
        (m_quality.has("debug.view") && m_quality.string("debug.view") != "none"))
    {
        m_upscaleValid = false;
        return;
    }
    const uint32_t W = v.width, H = v.height;
    const uint32_t w = std::max(8u, (uint32_t)std::lround((double)W * h / H));
    // 64 positions per internal pixel whatever the ratio: the upscale's narrow output-pixel kernel (output.upscale_kernel)
    // needs samples within about a tenth of an output pixel of every output centre (Upscale.hlsl).
    const uint32_t cycle = 64;
    const uint32_t k = (uint32_t)(frame.frameIndex % cycle) + 1;  // Halton from index 1 (index 0 is the origin)
    const float jx = halton(k, 2) - 0.5f, jy = halton(k, 3) - 0.5f;
    FrameContext::Upscale& u = frame.upscale;
    u.outputWidth = W;
    u.outputHeight = H;
    u.jitterX = jx;
    u.jitterY = jy;
    u.viewProj = v.viewProj;
    u.proj = v.proj;
    const bool sameSize = m_upscalePrevWidth == W && m_upscalePrevHeight == H;
    u.prevViewProj = m_upscaleValid && sameSize ? m_upscalePrevViewProj : v.viewProj;
    const float exposure = 1.0f / (1.2f * std::exp2(v.ev100));
    u.exposureRatio = m_upscaleValid && m_upscalePrevExposure > 0 ? exposure / m_upscalePrevExposure : 1.0f;
    u.reset = !m_upscaleValid || !sameSize || frame.discontinuity != 0;
    u.prevJitterX = u.reset ? jx : m_upscalePrevJitterX;
    u.prevJitterY = u.reset ? jy : m_upscalePrevJitterY;
    // The jittered projection: clip' = T clip with T translating NDC by (2 jx / w, -2 jy / h) (pixel +y is NDC -y), so
    // the content moves by (jx, jy) internal pixels: rows 0 and 1 of P gain the translation times row 3.
    const float tx = 2.0f * jx / (float)w, ty = -2.0f * jy / (float)h;
    for (int c = 0; c < 4; ++c)
    {
        v.proj.m[0][c] += tx * v.proj.m[3][c];
        v.proj.m[1][c] += ty * v.proj.m[3][c];
    }
    v.width = w;
    v.height = h;
    v.viewProj = mul(v.proj, v.view);
    v.invViewProj = inverse(v.viewProj);
    v.prevViewProj = u.reset ? v.viewProj : m_upscalePrevJittered;
    m_upscalePrevJittered = v.viewProj;
    m_upscalePrevViewProj = u.viewProj;
    m_upscalePrevExposure = exposure;
    m_upscalePrevJitterX = jx;
    m_upscalePrevJitterY = jy;
    m_upscalePrevWidth = W;
    m_upscalePrevHeight = H;
    m_upscaleValid = true;
}

ViewResources FrameRenderer::record(RenderGraph& graph, const FrameContext& in, TextureRef output)
{
    // History discontinuity (v1.35): no previous view in this frame; a restore also has no previous transforms or
    // palettes. The tracks reset their own temporal state from frame.discontinuity.
    FrameContext frame = in;
    if (frame.discontinuity != 0) frame.mainView.prevViewProj = frame.mainView.viewProj;
    if (frame.autoExposure)
    {
        if (!std::isfinite(frame.mainView.ev100)) frame.mainView.ev100 = 14.0f;  // (the host's starting value, if any)
        frame.mainView.ev100 = tracks::autoExposureEv100(m_trackState, m_device, m_quality, frame, m_framesInFlight);
    }
    ViewDesc outputView = frame.mainView;  // (the debug overlay's view when the frame upscales)
    setupUpscale(frame);  // (after the exposure: the history's exposure ratio)
    m_lastEv100 = frame.mainView.ev100;
    m_viewModelScale = tracks::viewModelPrepare(m_trackState, m_scene, m_quality, frame);  // E (A12): view models at this frame's camera
    if (frame.discontinuity & kDiscontinuityRestore) m_scene.resetMotion();
    // C9 origin rebase: the previous view is expressed in the new coordinates (a point p now was p + shift before).
    if (frame.originShift.x != 0 || frame.originShift.y != 0 || frame.originShift.z != 0)
    {
        for (float4x4* pvp : { &frame.mainView.prevViewProj, &frame.upscale.prevViewProj })
        {
            float4x4& pv = *pvp;
            for (int r = 0; r < 4; ++r) pv.m[r][3] += pv.m[r][0] * frame.originShift.x + pv.m[r][1] * frame.originShift.y + pv.m[r][2] * frame.originShift.z;
        }
    }
    m_scene.flushUpdates(frame.frameIndex, m_framesInFlight, m_shaders);  // transforms, palettes, visibility of this frame
    tracks::particleLightCapacity(m_trackState, m_scene);  // A3: before the imports and every frame constants
    FrameResources resources;
    importFxLights(graph, m_scene, resources);
    FrameServices services;
    FramePassContext fc{ m_device, graph, m_shaders, m_quality, m_scene, frame, resources, services,
                         [this, &frame](const ViewDesc& v) { return allocateFrameConstants(frame, v); }, &m_trackState, m_framesInFlight };
    m_debugDraw = 0xFFFFFFFFu;
    m_debugDraw = tracks::debugBegin(fc);  // E (A15): before any frame constants, which carry its buffer
    // Scene textures into the material records before any frame constants (they carry the material buffer's SRV).
    tracks::prepareScene(fc);
    tracks::lightFunctions(fc);  // E (A8): light function table and images, before every consumer
    services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
    services.traceRefractions = [](FramePassContext& c, BufferRef jobs, BufferRef results, uint32_t maxJobs) {
        tracks::refraction(c, jobs, results, maxJobs);
    };
    services.renderView = [](FramePassContext& c, const ViewDesc& v) {
        if (v.kind == gpu::ViewKind::Main) fail("renderView is for secondary views");
        ViewResources view;
        view.view = v;
        view.frameConstants = c.frameConstantsFor(v);
        view.color = c.graph.createTexture({ "secondary view colour", v.width, v.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        tracks::visibility(c, view);
        tracks::decals(c, view);
        tracks::materialResolve(c, view);
        tracks::giScreenIrradiance(c, view);  // R: the view's per-pixel cache irradiance (planar views, as the main view's)
        tracks::shadowVisibility(c, view);
        tracks::shading(c, view);
        return view;
    };

    ViewResources main;
    main.view = frame.mainView;
    main.frameConstants = fc.frameConstantsFor(main.view);
    main.color = output;
    std::vector<ViewResources> aux = auxiliaryViews(fc, frame);  // A14: in drawing order

    // ARCHITECTURE 4.1, one graphics queue (4.3). Order matters only through declared dependencies; it follows the
    // design so the reader can map passes to the budget table. A14 phases (Requests/20260926_C_per_view_history.md 5):
    // every view's visibility, the shadow pages once, the auxiliary views' shading, then the main view's resolve (its
    // materials read the auxiliary outputs of this frame).
    tracks::simulation(fc);  // C0
    tracks::particleMeshes(fc);  // A3 (render C): mesh particle instances, before V's culling
    tracks::particleLights(fc, main);  // A3: FX particle lights into the scene light tail, before S's lists
    tracks::waterGeometry(fc);  // W (B7/B8): ocean FFT and fluid surface into V's triangle streams
    tracks::atmosphere(fc);
    tracks::accelerationStructures(fc);
    tracks::hair(fc, main);  // E (B10): guide ticks and the frame's strand segments, before V
    for (ViewResources& v : aux) tracks::visibility(fc, v);
    tracks::visibility(fc, main);
    tracks::shadowPages(fc, main);  // reads V's products only (S, 2026-09-26); per-view marking: S (shadowMarkView)
    for (ViewResources& v : aux)
    {
        // The auxiliary chain of the planar reflection path; froxels, GI and reflections per view follow the tracks'
        // per-view generalisation (request section 5).
        tracks::decals(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shadowVisibility(fc, v);
        tracks::shading(fc, v);
    }
    tracks::decals(fc, main);  // E (A7): decal records and tile lists for the resolve
    tracks::surfaceState(fc);  // E (A7): the surface state field's changes
    tracks::materialResolve(fc, main);
    tracks::froxels(fc, main);
    main.froxelLights = resources.froxelLights;  // the main view's per-view S products (v1.22)
    main.airVolume = resources.aerialPerspective;
    tracks::globalIllumination(fc, main);
    tracks::reflections(fc, main);
    tracks::particles(fc, main);
    tracks::distortion(fc, main);
    tracks::shadowVisibility(fc, main);
    tracks::shading(fc, main);
    if (frame.upscale.outputWidth != 0 && m_debugDraw != 0xFFFFFFFFu)
    {
        // the overlay draws over the upscaled output: the output-size view (unjittered) and its own frame constants
        ViewResources overlay = main;
        overlay.view = outputView;
        overlay.frameConstants = fc.frameConstantsFor(outputView);
        tracks::debugOverlay(fc, overlay);
    }
    else
        tracks::debugOverlay(fc, main);  // E (A15): buffer visualization, debug primitives, HUD over the final colour
    return main;
}

void FrameRenderer::recordImage(RenderGraph& graph, const FrameContext& in, TextureRef image, TextureRef output, TextureRef sdrCopy)
{
    FrameContext frame = in;
    if (frame.discontinuity & kDiscontinuityRestore) m_scene.resetMotion();
    m_scene.flushUpdates(frame.frameIndex, m_framesInFlight, m_shaders);
    m_debugDraw = 0xFFFFFFFFu;
    m_viewModelScale = 1.0f;
    tracks::particleLightCapacity(m_trackState, m_scene);  // A3: before the imports and every frame constants
    FrameResources resources;
    importFxLights(graph, m_scene, resources);
    FrameServices services;
    FramePassContext fc{ m_device, graph, m_shaders, m_quality, m_scene, frame, resources, services,
                         [this, &frame](const ViewDesc& v) { return allocateFrameConstants(frame, v); }, &m_trackState, m_framesInFlight };
    ViewResources main;
    main.view = frame.mainView;
    main.frameConstants = fc.frameConstantsFor(main.view);
    main.color = output;
    tracks::imagePost(fc, main, image);
    if (!sdrCopy.valid()) return;
    FrameContext sdr = frame;
    sdr.displayPeak = 0;
    FramePassContext sc{ m_device, graph, m_shaders, m_quality, m_scene, sdr, resources, services,
                         [this, &frame](const ViewDesc& v) { return allocateFrameConstants(frame, v); }, &m_trackState, m_framesInFlight };
    ViewResources copy = main;
    copy.color = sdrCopy;
    tracks::imagePost(sc, copy, image);
}
} // namespace unx::render
