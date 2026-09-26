#include "unx/render/FrameRenderer.h"

#include "unx/render/Tracks.h"

#include <cmath>
#include <cstring>

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
    const gpu::FrameConstants c = frameConstants(m_scene, frame, view);
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
    m_lastEv100 = frame.mainView.ev100;
    if (frame.discontinuity & kDiscontinuityRestore) m_scene.resetMotion();
    m_scene.flushUpdates(frame.frameIndex, m_framesInFlight, m_shaders);  // transforms, palettes, visibility of this frame
    FrameResources resources;
    FrameServices services;
    FramePassContext fc{ m_device, graph, m_shaders, m_quality, m_scene, frame, resources, services,
                         [this, &frame](const ViewDesc& v) { return allocateFrameConstants(frame, v); }, &m_trackState, m_framesInFlight };
    // Scene textures into the material records before any frame constants (they carry the material buffer's SRV).
    tracks::prepareScene(fc);
    services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
    services.renderView = [](FramePassContext& c, const ViewDesc& v) {
        if (v.kind == gpu::ViewKind::Main) fail("renderView is for secondary views");
        ViewResources view;
        view.view = v;
        view.frameConstants = c.frameConstantsFor(v);
        view.color = c.graph.createTexture({ "secondary view colour", v.width, v.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        tracks::visibility(c, view);
        tracks::materialResolve(c, view);
        tracks::shadowVisibility(c, view);
        tracks::shading(c, view);
        return view;
    };

    ViewResources main;
    main.view = frame.mainView;
    main.frameConstants = fc.frameConstantsFor(main.view);
    main.color = output;

    // ARCHITECTURE 4.1, one graphics queue (4.3). Order matters only through declared dependencies; it follows the
    // design so the reader can map passes to the budget table.
    tracks::simulation(fc);  // C0
    tracks::atmosphere(fc);
    tracks::accelerationStructures(fc);
    tracks::visibility(fc, main);
    tracks::materialResolve(fc, main);
    tracks::shadowPages(fc, main);
    tracks::froxels(fc, main);
    main.froxelLights = resources.froxelLights;  // the main view's per-view S products (v1.22)
    main.airVolume = resources.aerialPerspective;
    tracks::globalIllumination(fc, main);
    tracks::reflections(fc, main);
    tracks::particles(fc, main);
    tracks::distortion(fc, main);
    tracks::shadowVisibility(fc, main);
    tracks::shading(fc, main);
    return main;
}
} // namespace unx::render
